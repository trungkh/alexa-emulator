/*
 * Copyright (c) 2016 Trung Huynh
 * All rights reserved
 */

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <curl/curl.h>
#include <mpg123.h>

#include <vector>

#include <pa_ringbuffer.h>
#include <pa_util.h>
#include <portaudio.h>

#include "configini/configini.h"
#include "json/jsmn.h"
#include "inc/snowboy-detect.h"

#include "alexa.h"

#define WAVE_FORMAT_PCM         0x0001
#define WAVE_FORMAT_IEEE_FLOAT  0x0003
#define BOUNDARY                "c9d341d3-0cce-4a55-ae8d-0d19ddda24f3"

#define ALEXA_SAMPLE_RATE       16000
#define BYTES_PER_SAMPLE        2
#define NUMBER_OF_CHANNEL       1

#define RING_BUFFER_SIZE        262144
#define RECORDING_TIME          3500   // mili-seconds

#define DATA_HEADER     "--%s\r\nContent-Disposition: form-data; name=\"metadata\"" \
                        "\r\nContent-Type: application/json; charset=UTF-8\r\n" \
                        "\r\n{\"messageHeader\":{  },\"messageBody\":{" \
                            "\"profile\":\"alexa-close-talk\"," \
                            "\"locale\":\"en-us\"," \
                            "\"format\":\"audio/L16; rate=%d; channels=1\"" \
                            "}}\r\n\r\n" \
                        "--%s\r\nContent-Disposition: form-data; name=\"audio\"" \
                        "\r\nContent-Type: audio/L16; rate=%d; channels=1\r\n\r\n"
#define DATA_TAILER     "\r\n\r\n--%s--\r\n\r\n"

static uint8_t debug;

static ring_buffer_size_t left_samples;
//static ring_buffer_size_t lost_samples;
static input_state is_in;

static pthread_mutex_t in_ring_mutex;
static pthread_mutex_t out_ring_mutex;
static pthread_mutex_t wav_mutex;
static pthread_cond_t wav_cond;

char *strnstr(char *string, const char *find, ssize_t len)
{
    char *cp;
    size_t find_len = strlen(find);
    for (cp = string; cp <= string + len - find_len; cp++)
    {
        if (!strncmp(cp, find, find_len))
            return cp;
    }
    return NULL;
}

char *strrstr(char *string, const char *find, ssize_t len)
{
    char *cp;
    size_t find_len = strlen(find);
    for (cp = string + len - find_len; cp >= string; cp--)
    {
        if (!strncmp(cp, find, find_len))
            return cp;
    }
    return NULL;
}

static void init_string(string_t *s)
{
    s->len = 0;
    s->ptr = (char *)malloc(s->len + 1);
    if (s->ptr == NULL)
    {
        fprintf(stderr, "init_string failed\n");
        exit(1);
    }
    s->ptr[0] = '\0';
}

static size_t writefunc(void *ptr, size_t size, size_t nmemb, string_t *s)
{
    if (debug)
    {
        printf("*** Write %ld bytes to file\n", size * nmemb);
    }

    size_t new_len = s->len + size * nmemb;
    s->ptr = (char *)realloc(s->ptr, new_len + 1);
    if (s->ptr == NULL)
    {
        fprintf(stderr, "realloc() failed\n");
        exit(1);
    }
    memcpy(s->ptr + s->len, ptr, size * nmemb);
    s->ptr[new_len] = '\0';
    s->len = new_len;

    return size * nmemb;
}

static size_t read_callback(void *ptr, size_t size, size_t nmemb, void *userp)
{
    data_stream_t *pooh = (data_stream_t *)userp;
    ring_buffer_size_t read_samples, available_samples;
    size_t actual_read;

    if(size * nmemb < 1)
        return 0;

    if (debug)
    {
        printf("*** Read %ld bytes from buffer size %ld\n", size * nmemb, pooh->sizeleft);
    }

    if(pooh->sizeleft >= (size * nmemb))
    {
        /*if (lost_samples > 0)
        {
            fprintf(stderr, "Lost %ld samples due to ring buffer overflow\n", lost_samples);
            lost_samples = 0;
        }*/

        while (1)
        {
            available_samples = PaUtil_GetRingBufferReadAvailable(pooh->pa_ring_buf);
            if (available_samples * (BYTES_PER_SAMPLE * NUMBER_OF_CHANNEL) >= size * nmemb)
            {
                break;
            }
            Pa_Sleep(10);
        }

        actual_read = (size * nmemb) / (BYTES_PER_SAMPLE * NUMBER_OF_CHANNEL);

        pthread_mutex_lock(&in_ring_mutex);
        read_samples = PaUtil_ReadRingBuffer(pooh->pa_ring_buf, ptr, actual_read);
        pthread_mutex_unlock(&in_ring_mutex);
        if (read_samples != actual_read)
        {
            fprintf(stderr, "%ld samples were available, but only %ld samples were read\n",
                    available_samples, read_samples);
        }

        pooh->sizeleft -= (read_samples * (BYTES_PER_SAMPLE * NUMBER_OF_CHANNEL));
        return (read_samples * (BYTES_PER_SAMPLE * NUMBER_OF_CHANNEL));
    }
    else if (pooh->sizeleft > 0)
    {
        /*if (lost_samples > 0)
        {
            fprintf(stderr, "Lost %ld samples due to ring buffer overflow\n", lost_samples);
            lost_samples = 0;
        }*/

        while (1)
        {
            available_samples = PaUtil_GetRingBufferReadAvailable(pooh->pa_ring_buf);
            if (available_samples * (BYTES_PER_SAMPLE * NUMBER_OF_CHANNEL) >= pooh->sizeleft)
            {
                break;
            }
            Pa_Sleep(10);
        }

        actual_read = pooh->sizeleft / (BYTES_PER_SAMPLE * NUMBER_OF_CHANNEL);

        pthread_mutex_lock(&in_ring_mutex);
        read_samples = PaUtil_ReadRingBuffer(pooh->pa_ring_buf, ptr, actual_read);
        pthread_mutex_unlock(&in_ring_mutex);
        if (read_samples != actual_read)
        {
            fprintf(stderr, "%ld samples were available, but only %ld samples were read\n",
                    available_samples, read_samples);
        }

        pooh->sizeleft -= (read_samples * (BYTES_PER_SAMPLE * NUMBER_OF_CHANNEL));
        return (read_samples * (BYTES_PER_SAMPLE * NUMBER_OF_CHANNEL));
    }

    return 0; /* no more data left to deliver */
}

static void json_get_str(jsmntok_t *t, const char *data, char *name)
{
    int len = t->end - t->start;
    memcpy(name, data + t->start, len);
    name[len] = '\0';
}

static void json_get_int(jsmntok_t *t, const char *data, int *number)
{
    char str[10] = {'\0'};
    int len = t->end - t->start;
    memcpy(str, data + t->start, len);

    *number = atoi(str);
}

static void wav_format_init(unsigned short *bitspersample, unsigned short *wavformat, int enc)
{
    if(enc & MPG123_ENC_FLOAT_64)
    {
        *bitspersample = 64;
        *wavformat = WAVE_FORMAT_IEEE_FLOAT;
    }
    else if(enc & MPG123_ENC_FLOAT_32)
    {
        *bitspersample = 32;
        *wavformat = WAVE_FORMAT_IEEE_FLOAT;
    }
    else if(enc & MPG123_ENC_16)
    {
        *bitspersample = 16;
        *wavformat = WAVE_FORMAT_PCM;
    }
    else
    {
        *bitspersample = 8;
        *wavformat = WAVE_FORMAT_PCM;
    }
}

static void wav_file_init(FILE *out, size_t *totaloffset, size_t *dataoffset,
        unsigned short bitspersample, unsigned short wavformat, long rate, int channels)
{
    unsigned int tmp32 = 0;
    unsigned short tmp16 = 0;

    fwrite("RIFF", 1, 4, out);
    *totaloffset = ftell(out);

    fwrite(&tmp32, 1, 4, out); // total size
    fwrite("WAVE", 1, 4, out);
    fwrite("fmt ", 1, 4, out);

    tmp32 = 16;
    fwrite(&tmp32, 1, 4, out); // format length
    tmp16 = wavformat;
    fwrite(&tmp16, 1, 2, out); // format
    tmp16 = channels;
    fwrite(&tmp16, 1, 2, out); // channels
    tmp32 = rate;
    fwrite(&tmp32, 1, 4, out); // sample rate
    tmp32 = rate * bitspersample/8 * channels;
    fwrite(&tmp32, 1, 4, out); // bytes / second
    tmp16 = bitspersample/8 * channels; // float 16 or signed int 16
    fwrite(&tmp16, 1, 2, out); // block align
    tmp16 = bitspersample;
    fwrite(&tmp16, 1, 2, out); // bits per sample

    fwrite("data", 1, 4, out);
    *dataoffset = ftell(out);

    tmp32 = 0;
    fwrite(&tmp32, 1, 4, out); // data length
}

static int wav_file_read(FILE *in, size_t *fsize)
{
    char buf[5] = {0};
    while (1)
    {
        fread(buf, 1, 4, in); *fsize -= 4;
        if (!strcmp(buf, "RIFF"))
        {
            fseek(in, 4, SEEK_CUR); *fsize -= 4;
        }
        else if (!strcmp(buf, "WAVE"))
        {
            //do nothing
        }
        else if (!strcmp(buf, "fmt "))
        {
            unsigned short bitspersample;
            unsigned short wavformat;
            long rate;
            int channels;

            fseek(in, 4, SEEK_CUR); *fsize -= 4; // format length
            fread(&wavformat, 1, 2, in); *fsize -= 2;
            fread(&channels, 1, 2, in); *fsize -= 2;
            fread(&rate, 1, 4, in); *fsize -= 4;
            fseek(in, 4, SEEK_CUR); *fsize -= 4; // bytes / second
            fseek(in, 2, SEEK_CUR); *fsize -= 2; // block align: float 16 or signed int 16
            fread(&bitspersample, 1, 2, in); *fsize -= 2;

            if(channels != NUMBER_OF_CHANNEL || bitspersample != (BYTES_PER_SAMPLE * 8))
            {
                fprintf(stderr, "Wrong format to play\n");
                return 1;
            }
        }
        else if (!strcmp(buf, "data"))
        {
            size_t len = 0;
            fread(&len, 1, 4, in); *fsize -= 4;
            if (*fsize != len)
                fprintf(stderr, "File size mismatch with format length\n");
            return 0;
        }
        else
        {
            fprintf(stderr, "Maybe raw file, or not support this file yet!\n");
            return 1;
        }
    }
    return 1;
}

static void wav_file_close(FILE *out, size_t totaloffset, size_t dataoffset)
{
    unsigned int tmp32 = 0;
    long total = ftell(out);

    fseek(out, totaloffset, SEEK_SET);
    tmp32 = total - (totaloffset + 4);
    fwrite(&tmp32, 1, 4, out);

    fseek(out, dataoffset, SEEK_SET);
    tmp32 = total - (dataoffset + 4);
    fwrite(&tmp32, 1, 4, out);

    fseek(out, 0, SEEK_END);
}

static int wav_buffer_init(char *buf, unsigned short bitspersample,
        unsigned short wavformat, long rate, int channels)
{
    char *total_ptr, *data_ptr, *ptr = buf;
    unsigned int tmp32 = 0;
    unsigned short tmp16 = 0;
    unsigned int total = RECORDING_TIME * ALEXA_SAMPLE_RATE * (BYTES_PER_SAMPLE * NUMBER_OF_CHANNEL) / 1000;

    sprintf(ptr, "RIFF"); ptr += 4;
    total_ptr = ptr; ptr += 4;          // save total size ptr

    sprintf(ptr, "WAVE"); ptr += 4;
    sprintf(ptr, "fmt "); ptr += 4;

    tmp32 = 16;
    memcpy(ptr, &tmp32, 4); ptr += 4;   // format length
    tmp16 = wavformat;
    memcpy(ptr, &tmp16, 2); ptr += 2;   // format
    tmp16 = channels;
    memcpy(ptr, &tmp16, 2); ptr += 2;   // channels
    tmp32 = rate;
    memcpy(ptr, &tmp32, 4); ptr += 4;   // sample rate
    tmp32 = rate * bitspersample/8 * channels;
    memcpy(ptr, &tmp32, 4); ptr += 4;   // bytes / second
    tmp16 = bitspersample/8 * channels; // float 16 or signed int 16
    memcpy(ptr, &tmp16, 2); ptr += 2;   // block align
    tmp16 = bitspersample;
    memcpy(ptr, &tmp16, 2); ptr += 2;   // bits per sample

    sprintf(ptr, "data"); ptr += 4;
    data_ptr = ptr; ptr += 4;           // save data length ptr

    total += (ptr - buf);
    tmp32 = total - (total_ptr - buf + 4);
    memcpy(total_ptr, &tmp32, 4);       // total size
    tmp32 = total - (data_ptr - buf + 4);
    memcpy(data_ptr, &tmp32, 4);        // data length

    return (ptr - buf);
}

static int get_refresh_token(const char *client_id, const char *client_secret, const char *code,
        char *refresh_token, char *access_token, int *expired_in)
{
    CURL *curl;
    char getRefreshToken[] = "https://api.amazon.com/auth/o2/token";
    char error_str[128];

    curl = curl_easy_init();
    if(curl)
    {
        struct curl_slist *chunk = NULL;
        char content[512] = {'\0'};
        string_t response;
        CURLcode res;

        int ret, i;
        jsmn_parser p;
        char name[32];
        jsmntok_t t[16]; /* We expect no more than 128 tokens */

        init_string(&response);

        sprintf(content, "grant_type=authorization_code&code=%s&client_id=%s&"
                "client_secret=%s&redirect_uri=https://localhost",
                code, client_id, client_secret);

        chunk = curl_slist_append(chunk, "Host: api.amazon.com");
        chunk = curl_slist_append(chunk, "Content-Type: application/x-www-form-urlencoded");
        chunk = curl_slist_append(chunk, "Cache-Control: no-cache");

        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, content);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writefunc);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0);
        curl_easy_setopt(curl, CURLOPT_URL, getRefreshToken);
        if (debug)
        {
            curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
        }
        else
        {
            curl_easy_setopt(curl, CURLOPT_VERBOSE, 0);
        }

        if (debug)
        {
            printf("Body: %s\n", content);
        }

        res = curl_easy_perform(curl);
        curl_slist_free_all(chunk);
        curl_easy_cleanup(curl);

        if(res != CURLE_OK)
        {
            fprintf(stderr, "get_refresh_token failed: %s\n", curl_easy_strerror(res));
            return res;
        }

        jsmn_init(&p);
        ret = jsmn_parse(&p, response.ptr, strlen(response.ptr), t, sizeof(t) / sizeof(t[0]));
        if(ret < 0)
        {
            printf("Failed to parse JSON: %d\n", ret);
            ret = 1;
        }
        else if(ret < 1 || t[0].type != JSMN_OBJECT)
        {
            printf("Object expected\n");
            ret = 1;
        }
        else
        {
            for (i = 0; i < ret; i++)
            {
                if(t[i].type == JSMN_STRING && t[i].size == 1) // must be key
                {
                    json_get_str(&t[i], response.ptr, name);
                    if (!strcmp(name, "access_token"))
                    {
                        json_get_str(&t[++i], response.ptr, access_token);
                    }
                    else if (!strcmp(name, "refresh_token"))
                    {
                        json_get_str(&t[++i], response.ptr, refresh_token);
                    }
                    else if (!strcmp(name, "expires_in"))
                    {
                        json_get_int(&t[++i], response.ptr, expired_in);
                    }
                    else if (!strcmp(name, "error"))
                    {
                        json_get_str(&t[++i], response.ptr, error_str);
                        printf("Error: %s", error_str);
                    }
                }
            }
            ret = 0;
        }

        free(response.ptr);
        return ret;
    }
    return 1;
}

static int get_access_token(const char *client_id, const char *client_secret,
        char *refresh_token, char *access_token, int *expired_in)
{
    CURL *curl;
    char getAccessToken[] = "https://api.amazon.com/auth/o2/token";
    char error_str[128];

    curl = curl_easy_init();
    if(curl)
    {
        struct curl_slist *chunk = NULL;
        char content[1024] = {'\0'};
        string_t response;
        CURLcode res;

        int ret, i;
        jsmn_parser p;
        char name[32];
        jsmntok_t t[16]; /* We expect no more than 128 tokens */

        init_string(&response);

        sprintf(content, "grant_type=refresh_token&refresh_token=%s&client_id=%s&client_secret=%s",
                refresh_token, client_id, client_secret);

        chunk = curl_slist_append(chunk, "Host: api.amazon.com");
        chunk = curl_slist_append(chunk, "Content-Type: application/x-www-form-urlencoded");
        chunk = curl_slist_append(chunk, "Cache-Control: no-cache");

        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, content);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writefunc);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0);
        curl_easy_setopt(curl, CURLOPT_URL, getAccessToken);
        if (debug)
        {
            curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
        }
        else
        {
            curl_easy_setopt(curl, CURLOPT_VERBOSE, 0);
        }

        if (debug)
        {
            printf("Body: %s\n", content);
        }

        res = curl_easy_perform(curl);
        curl_slist_free_all(chunk);
        curl_easy_cleanup(curl);

        if(res != CURLE_OK)
        {
            fprintf(stderr, "get_access_token failed: %s\n", curl_easy_strerror(res));
            return res;
        }

        jsmn_init(&p);
        ret = jsmn_parse(&p, response.ptr, strlen(response.ptr), t, sizeof(t) / sizeof(t[0]));
        if(ret < 0)
        {
            printf("Failed to parse JSON: %d\n", ret);
            ret = 1;
        }
        else if(ret < 1 || t[0].type != JSMN_OBJECT)
        {
            printf("Object expected\n");
            ret = 1;
        }
        else
        {
            for (i = 0; i < ret; i++)
            {
                if(t[i].type == JSMN_STRING && t[i].size == 1) // must be key
                {
                    json_get_str(&t[i], response.ptr, name);
                    if (!strcmp(name, "access_token"))
                    {
                        json_get_str(&t[++i], response.ptr, access_token);
                    }
                    else if (!strcmp(name, "refresh_token"))
                    {
                        json_get_str(&t[++i], response.ptr, refresh_token);
                    }
                    else if (!strcmp(name, "expires_in"))
                    {
                        json_get_int(&t[++i], response.ptr, expired_in);
                    }
                    else if (!strcmp(name, "error"))
                    {
                        json_get_str(&t[++i], response.ptr, error_str);
                        printf("Error: %s", error_str);
                    }
                }
            }
            ret = 0;
        }

        free(response.ptr);
        return ret;
    }
    return 1;
}

static ring_buffer_size_t stream_read(PaUtilRingBuffer *pa_ring_buf, std::vector<int16_t>* data)
{
    ring_buffer_size_t read_samples, available_samples = 0;
    /*if (lost_samples > 0)
    {
        fprintf(stderr, "Lost %ld samples due to ring buffer overflow\n", lost_samples);
        lost_samples = 0;
    }*/

    while (1)
    {
        available_samples = PaUtil_GetRingBufferReadAvailable(pa_ring_buf);
        if (available_samples >= (ALEXA_SAMPLE_RATE * 0.1))
        {
            break;
        }
        Pa_Sleep(10);
    }

    data->resize(available_samples);
    pthread_mutex_lock(&in_ring_mutex);
    read_samples = PaUtil_ReadRingBuffer(pa_ring_buf, data->data(), available_samples);
    pthread_mutex_unlock(&in_ring_mutex);
    if (read_samples != available_samples)
    {
        fprintf(stderr, "%ld samples were available, but only %ld samples were read\n",
                available_samples, read_samples);
    }

    return read_samples;
}

static void *create_multipart_wav_buffer(void *userData)
{
    data_stream_t *pooh = (data_stream_t *)userData;
    ring_buffer_size_t read_samples, written_samples;
    size_t total_size = 0;
    char buffer[MAXBUF];

    pthread_mutex_lock(&in_ring_mutex);
    PaUtil_FlushRingBuffer(pooh->pa_ring_buf);
    sprintf(buffer, DATA_HEADER, BOUNDARY, ALEXA_SAMPLE_RATE, BOUNDARY, ALEXA_SAMPLE_RATE);

    written_samples = PaUtil_WriteRingBuffer(pooh->pa_ring_buf, buffer,
            strlen(buffer) / (BYTES_PER_SAMPLE * NUMBER_OF_CHANNEL));
    //lost_samples += strlen(buffer) / (g_bytes_per_sample * g_num_channels) - written_samples;
    total_size += written_samples * (BYTES_PER_SAMPLE * NUMBER_OF_CHANNEL);

    read_samples = wav_buffer_init(buffer, BYTES_PER_SAMPLE * 8,
            WAVE_FORMAT_PCM, ALEXA_SAMPLE_RATE, NUMBER_OF_CHANNEL);
    written_samples = PaUtil_WriteRingBuffer(pooh->pa_ring_buf, buffer,
            read_samples / (BYTES_PER_SAMPLE * NUMBER_OF_CHANNEL));
    //lost_samples += read_samples / (g_bytes_per_sample * g_num_channels) - written_samples;
    total_size += written_samples * (BYTES_PER_SAMPLE * NUMBER_OF_CHANNEL);

    left_samples = RECORDING_TIME * ALEXA_SAMPLE_RATE / 1000;
    total_size += left_samples * (BYTES_PER_SAMPLE * NUMBER_OF_CHANNEL);

    is_in = RECORD_INPUT;
    pthread_mutex_unlock(&in_ring_mutex);

    sprintf(buffer, DATA_TAILER, BOUNDARY);
    total_size += strlen(buffer);

    pooh->sizeleft = total_size;
    pthread_cond_signal(&wav_cond);

    while(is_in == RECORD_INPUT)
    {
        usleep(10000);
    }

    pthread_mutex_lock(&in_ring_mutex);
    written_samples = PaUtil_WriteRingBuffer(pooh->pa_ring_buf, buffer,
            strlen(buffer) / (BYTES_PER_SAMPLE * NUMBER_OF_CHANNEL));
    //lost_samples += strlen(buffer) / (g_bytes_per_sample * g_num_channels) - written_samples;
    pthread_mutex_unlock(&in_ring_mutex);

    return NULL;
}

static int speech_request(const char *access_token, char **resp, int *length, PaUtilRingBuffer *pa_ring_buf)
{
    char speechRequestLink[] = "https://access-alexa-na.amazon.com/v1/avs/speechrecognizer/recognize";
    CURL *curl;

    curl = curl_easy_init();
    if(curl)
    {
        struct curl_slist *chunk = NULL;
        char header_token[1024] = {'\0'};
        char header_type[128] = {'\0'};
        pthread_t thread_wav;
        data_stream_t pooh;
        string_t response;
        string_t header;
        CURLcode res;
        char *ptr;
        int code;

        pooh.pa_ring_buf = pa_ring_buf;
        pooh.sizeleft = 0;

        pthread_create (&thread_wav, NULL, &create_multipart_wav_buffer, (void *)&pooh);
        while(!pooh.sizeleft) pthread_cond_wait(&wav_cond, &wav_mutex);

        memset(&header, 0, sizeof(string_t));
        memset(&response, 0, sizeof(string_t));
        init_string(&header);
        init_string(&response);

        sprintf(header_token, "Authorization: Bearer %s", access_token);
        sprintf(header_type, "Content-Type: multipart/form-data; boundary=%s", BOUNDARY);

        chunk = curl_slist_append(chunk, "Host: access-alexa-na.amazon.com");
        chunk = curl_slist_append(chunk, header_token);
        chunk = curl_slist_append(chunk, header_type);
        chunk = curl_slist_append(chunk, "Transfer-Encoding: chunked");

        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_callback);
        curl_easy_setopt(curl, CURLOPT_READDATA, &pooh);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, pooh.sizeleft);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writefunc);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, writefunc);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &header);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0);
        curl_easy_setopt(curl, CURLOPT_URL, speechRequestLink);
        if (debug)
        {
            curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
        }
        else
        {
            curl_easy_setopt(curl, CURLOPT_VERBOSE, 0);
        }

        res = curl_easy_perform(curl);
        curl_slist_free_all(chunk);
        curl_easy_cleanup(curl);

        pthread_join(thread_wav, NULL);

        if(res != CURLE_OK)
        {
            fprintf(stderr, "upload_file failed: %s\n", curl_easy_strerror(res));
            if (res == CURLE_OPERATION_TIMEDOUT)
            {
                if (header.ptr) free(header.ptr);
                if (response.ptr) free(response.ptr);
            }
            pthread_mutex_lock(&in_ring_mutex);
            PaUtil_FlushRingBuffer(pa_ring_buf);
            pthread_mutex_unlock(&in_ring_mutex);
            return res;
        }

        ptr = strrstr(header.ptr, "HTTP/1.1", strlen(header.ptr));
        sscanf(ptr + 8, "%d", &code);
        free(header.ptr);

        *resp = response.ptr;
        *length = response.len;

        if (code != 200)
        {
            fprintf(stderr, "Response nothing with code %d\n", code);
            return -1;
        }
        return 0;
    }
    return -1;
}

int pa_stream_callback(
    const void *input,
    void *output,
    unsigned long frameCount,
    const PaStreamCallbackTimeInfo* timeInfo,
    PaStreamCallbackFlags statusFlags,
    void *userData )
{
    ring_buffer_size_t read_samples, written_samples, available_samples;
    ring_buf_t *fifo = (ring_buf_t *)userData;

    available_samples = PaUtil_GetRingBufferReadAvailable(&fifo->pa_output_ring_buf);
    if (available_samples >= frameCount)
    {
        pthread_mutex_lock(&out_ring_mutex);
        read_samples = PaUtil_ReadRingBuffer(&fifo->pa_output_ring_buf, output, frameCount);
        pthread_mutex_unlock(&out_ring_mutex);
        output = (uint8_t*)output + read_samples * BYTES_PER_SAMPLE * NUMBER_OF_CHANNEL;
        frameCount -= read_samples;

        if(frameCount > 0)
        {
            memset(output, 0, frameCount * BYTES_PER_SAMPLE * NUMBER_OF_CHANNEL);
        }
    }

    available_samples = PaUtil_GetRingBufferWriteAvailable(&fifo->pa_input_ring_buf);
    if (available_samples >= frameCount && is_in != STOP_INPUT)
    {
        pthread_mutex_lock(&in_ring_mutex);
        if (is_in == REAL_TIME_INPUT)
            written_samples = PaUtil_WriteRingBuffer(&fifo->pa_input_ring_buf, input, frameCount);
        else if (is_in == RECORD_INPUT)
            written_samples = PaUtil_WriteRingBuffer(&fifo->pa_input_ring_buf, input,
                    (frameCount > left_samples) ? left_samples : frameCount);
        //lost_samples += frameCount - written_samples;
        pthread_mutex_unlock(&in_ring_mutex);
        if (is_in == RECORD_INPUT)
        {
            left_samples >= written_samples ? left_samples -= written_samples : left_samples = 0;
            if (!left_samples)
            {
                is_in = STOP_INPUT;
            }
        }
    }

    return paContinue;
}

static int stream_init(PaStream **pa_stream, ring_buf_t *fifo)
{
    PaError pa_err;
    ring_buffer_size_t rb_init_ans;

    fifo->in_ring_buf = (char *)PaUtil_AllocateMemory(RING_BUFFER_SIZE * BYTES_PER_SAMPLE * NUMBER_OF_CHANNEL);
    if (fifo->in_ring_buf == NULL)
    {
        fprintf(stderr, "Fail to allocate memory for ring buffer\n");
        return 1;
    }

    fifo->out_ring_buf = (char *)PaUtil_AllocateMemory(RING_BUFFER_SIZE * BYTES_PER_SAMPLE * NUMBER_OF_CHANNEL);
    if (fifo->out_ring_buf == NULL)
    {
        fprintf(stderr, "Fail to allocate memory for ring buffer\n");
        goto __STREAM_IN_FREE;
    }

    rb_init_ans = PaUtil_InitializeRingBuffer(&fifo->pa_input_ring_buf,
            BYTES_PER_SAMPLE * NUMBER_OF_CHANNEL, RING_BUFFER_SIZE, (void *)fifo->in_ring_buf);
    if (rb_init_ans == -1)
    {
        fprintf(stderr, "Inner ring buffer size is not power of 2\n");
        goto __STREAM_OUT_FREE;
    }

    rb_init_ans = PaUtil_InitializeRingBuffer(&fifo->pa_output_ring_buf,
            BYTES_PER_SAMPLE * NUMBER_OF_CHANNEL, RING_BUFFER_SIZE, (void *)fifo->out_ring_buf);
    if (rb_init_ans == -1)
    {
        fprintf(stderr, "Outer ring buffer size is not power of 2\n");
        goto __STREAM_OUT_FREE;
    }

    pa_err = Pa_Initialize();
    if (pa_err != paNoError)
    {
        fprintf(stderr, "Fail to initialize PortAudio, error message is: %s\n",
                Pa_GetErrorText(pa_err));
        goto __STREAM_OUT_FREE;
    }

    pa_err = Pa_OpenDefaultStream(
                pa_stream, 1, 1, paInt16, ALEXA_SAMPLE_RATE,
                paFramesPerBufferUnspecified, pa_stream_callback, (void *)fifo);

    if (pa_err != paNoError)
    {
        fprintf(stderr, "Fail to open PortAudio stream, error message is: %s\n",
                Pa_GetErrorText(pa_err));
        goto __STREAM_TERMINATE;
    }

    pa_err = Pa_StartStream(*pa_stream);
    if (pa_err != paNoError)
    {
        fprintf(stderr, "Fail to start PortAudio stream, error message is: %s\n",
                Pa_GetErrorText(pa_err));
        goto __STREAM_CLOSE;
    }
    return 0;

__STREAM_CLOSE:
    Pa_CloseStream(*pa_stream);
__STREAM_TERMINATE:
    Pa_Terminate();
__STREAM_OUT_FREE:
    if (fifo->out_ring_buf != NULL)
        PaUtil_FreeMemory(fifo->out_ring_buf);
__STREAM_IN_FREE:
    if (fifo->in_ring_buf != NULL)
        PaUtil_FreeMemory(fifo->in_ring_buf);
    return 1;
}

static int stream_close(PaStream *pa_stream, ring_buf_t *fifo)
{
    PaError pa_err = Pa_StopStream(pa_stream);
    if (pa_err != paNoError)
    {
        fprintf(stderr, "Fail to stop PortAudio stream, error message is: %s\n",
                Pa_GetErrorText(pa_err));
        return 1;
    }

    pa_err = Pa_CloseStream(pa_stream);
    if (pa_err != paNoError)
    {
        fprintf(stderr, "Fail to close PortAudio stream, error message is: %s\n",
                Pa_GetErrorText(pa_err));
        return 1;
    }

    Pa_Terminate();

    if (fifo->in_ring_buf != NULL)
        PaUtil_FreeMemory(fifo->in_ring_buf);
    if (fifo->out_ring_buf != NULL)
        PaUtil_FreeMemory(fifo->out_ring_buf);

    return 0;
}

static int stream_write(const char *output, PaUtilRingBuffer *pa_ring_buf, const char *ptr, size_t len)
{
    FILE *out = NULL;
    int ret = 0;

    if (output != NULL)
        out = fopen(output, "wb");

    if (output != NULL && out == NULL)
    {
        fprintf(stderr, "Open file %s failed\n", output);
        ret = 1;
    }
    else
    {
        mpg123_handle *mh;
        size_t done;
        off_t frame_offset;
        int err;

        unsigned char *audio;
        int channels, encoding;
        long rate;

        size_t totaloffset, dataoffset;
        unsigned short bitspersample, wavformat;

        mpg123_init();
        mh = mpg123_new(NULL, &err);
        if(mh == NULL)
        {
            fprintf(stderr,"Unable to create mpg123 handle: %s\n", mpg123_plain_strerror(err));
            ret = 1;
            goto __MPG_EXIT;
        }

        err = mpg123_format_none(mh);
        if(err != MPG123_OK)
        {
            fprintf(stderr,"Unable to disable all output formats: %s\n", mpg123_plain_strerror(err));
            ret = 1;
            goto __MPG_DELETE;
        }

        err = mpg123_format(mh, ALEXA_SAMPLE_RATE, MPG123_MONO,  MPG123_ENC_SIGNED_16);
        if(err != MPG123_OK)
        {
            fprintf(stderr,"Unable to set float output formats: %s\n", mpg123_plain_strerror(err));
            ret = 1;
            goto __MPG_DELETE;
        }

        err = mpg123_open_feed(mh);
        if(err != MPG123_OK)
        {
            fprintf(stderr,"Unable open feed: %s\n", mpg123_plain_strerror(err));
            ret = 1;
            goto __MPG_DELETE;
        }

        err = mpg123_feed(mh, (const unsigned char*) (ptr), len);
        if(err == MPG123_ERR)
        {
            fprintf(stderr, "Error: %s", mpg123_strerror(mh));
            ret = 1;
            goto __MPG_CLOSE;
        }

        do
        {
            err = mpg123_decode_frame(mh, &frame_offset, &audio, &done);
            switch(err)
            {
                case MPG123_NEW_FORMAT:
                    if (out != NULL)
                    {
                        mpg123_getformat(mh, &rate, &channels, &encoding);
                        wav_format_init(&bitspersample, &wavformat, encoding);
                        wav_file_init(out, &totaloffset, &dataoffset, bitspersample, wavformat, rate, channels);
                        //printf("New format: %li Hz, %i channels, encoding value %i\n", rate, channels, encoding);
                    }
                    break;
                case MPG123_OK:
                    if (out != NULL)
                    {
                        fwrite(audio, 1, done, out);
                    }
                    {
                        ring_buffer_size_t available_samples;
                        while (1)
                        {
                            available_samples = PaUtil_GetRingBufferWriteAvailable(pa_ring_buf);
                            if (available_samples >= done / (BYTES_PER_SAMPLE * NUMBER_OF_CHANNEL))
                            {
                                break;
                            }
                            Pa_Sleep(10);
                        }

                        pthread_mutex_lock(&out_ring_mutex);
                        PaUtil_WriteRingBuffer(pa_ring_buf, audio, done / (BYTES_PER_SAMPLE * NUMBER_OF_CHANNEL));
                        pthread_mutex_unlock(&out_ring_mutex);
                    }
                    break;
                case MPG123_NEED_MORE:
                    break;
                default:
                    break;
            }
        } while(err != MPG123_ERR && err != MPG123_NEED_MORE);

        if (out != NULL)
        {
            wav_file_close(out, totaloffset, dataoffset);
        }
__MPG_CLOSE:
        mpg123_close(mh);
__MPG_DELETE:
        mpg123_delete(mh);
__MPG_EXIT:
        mpg123_exit();

        if (out != NULL)
        {
            fclose(out);
        }
    }
    return ret;
}

int is_reask(const char *json_ptr, size_t json_size)
{
    int ret, i;
    jsmn_parser p;
    char name[32];
    jsmntok_t t[64];

    jsmn_init(&p);
    ret = jsmn_parse(&p, json_ptr, json_size, t, sizeof(t) / sizeof(t[0]));
    if(ret < 0)
    {
        printf("Failed to parse JSON: %d\n", ret);
        ret = -1;
    }
    else if(ret < 1 || t[0].type != JSMN_OBJECT)
    {
        printf("Object expected\n");
        ret = -1;
    }
    else
    {
        for (i = 0; i < ret; i++)
        {
            if(t[i].type == JSMN_STRING && t[i].size == 1) // must be key
            {
                json_get_str(&t[i], json_ptr, name);
                if (!strcmp(name, "namespace"))
                {
                    json_get_str(&t[++i], json_ptr, name);
                    if (!strcmp(name, "SpeechRecognizer"))
                    {
                        return 1;
                    }
                }
            }
        }
        ret = 0;
    }
    return ret;
}

int load_config(Config *cfg, char *conf_file, alexa_config_t *config, time_t *now)
{
    char code[64] = {'\0'};
    int ret = 0;

    memset(config, 0, sizeof(alexa_config_t));
    ConfigReadString(cfg, ALEXA_SECTION, ALEXA_CLIENT_ID, config->client_id, sizeof(config->client_id), "");
    ConfigReadString(cfg, ALEXA_SECTION, ALEXA_CLIENT_SECRET, config->client_secret, sizeof(config->client_secret), "");
    ConfigReadString(cfg, ALEXA_SECTION, ALEXA_REFRESH_TOKEN, config->refresh_token, sizeof(config->refresh_token), "");
    ConfigReadString(cfg, ALEXA_SECTION, ALEXA_ACCESS_TOKEN, config->access_token, sizeof(config->access_token), "");
    ConfigReadUnsignedInt(cfg, ALEXA_SECTION, ALEXA_CREATED_TIME, &config->created_time, 0);
    ConfigReadInt(cfg, ALEXA_SECTION, ALEXA_EXPIRED_IN, &config->expired_in, 0);

    if (!strlen(config->refresh_token))
    {
        char dev_type_id[] = "Camera0081";
        char dev_serial_num[] = "123456";

        printf("Please open the following URL in your browser and follow the steps until you see a blank page:");
        printf("https://www.amazon.com/ap/oa?client_id=%s&scope=alexa%%3Aall&"
                "scope_data=%%7B%%22alexa%%3Aall%%22%%3A%%20%%7B%%22productID%%22%%3A%%20%%22%s%%22%%2C%%20%%22"
                "productInstanceAttributes%%22%%3A%%20%%7B%%22deviceSerialNumber%%22%%3A%%20%%22%s%%22%%7D%%7D%%7D&"
                "response_type=code&redirect_uri=https%%3A%%2F%%2Flocalhost", config->client_id, dev_type_id, dev_serial_num);

        printf("\nWhen ready, please enter the value of the code parameter (from the URL of the blank page) and press enter\n");
        printf("Code: ");
        scanf("%s", code);

        if (!get_refresh_token(config->client_id, config->client_secret, code, config->refresh_token, config->access_token, &config->expired_in))
        {
            if (!strlen(config->refresh_token))
            {
                printf("Cannot get refresh token\n");
                ret = 1;
                goto __LOAD_EXIT;
            }
            else
            {
                config->created_time = *now;
                ConfigAddString(cfg, ALEXA_SECTION, ALEXA_REFRESH_TOKEN, config->refresh_token);
                ConfigAddString(cfg, ALEXA_SECTION, ALEXA_ACCESS_TOKEN, config->access_token);
                ConfigAddUnsignedInt(cfg, ALEXA_SECTION, ALEXA_CREATED_TIME, config->created_time);
                ConfigAddInt(cfg, ALEXA_SECTION, ALEXA_EXPIRED_IN, config->expired_in);
                ConfigPrintToFile(cfg, conf_file);
            }
        }
        else
        {
            printf("Get refresh token failed\n");
            ret = 1;
            goto __LOAD_EXIT;
        }
    }

__LOAD_EXIT:
    return ret;
}

void usage(const char *name)
{
    printf("The app can be used to speak command to alexa.\n");
    printf("Usage:\n %s -c <config_file> [options..]\n", name);
    printf("Options:\n");
    printf("-c | --config <config_file>  Default config file with custom config file.\n");
    //printf("-m | --model <umdl_file>     Model file which user want to call.\n");
    //printf("-r | --res <resource_input>  Resource file.\n");
    printf("-s | --sound <listen_sound>  Sound file to confirm alexa ready to listen.\n");
    printf("-l | --lost <lost_sound>     Sound file to confirm alexa lost connection.\n");
    printf("-o | --output <audio_output> Audio output file with response from alexa.\n");
    printf("-v | --verbose               Display detailed message.\n");
    printf("-h | --help                  Display usage instructions.\n");
}

int main(int argc, char *argv[])
{
    Config *cfg = NULL;
    alexa_config_t config;

    char *conf_file;
    //char *model_input = NULL;
    //char *res_input = NULL;
    char *listen_sound = NULL;
    char *lost_sound = NULL;
    char *audio_output = NULL;
    size_t sound_size = 0;
    size_t lost_size = 0;

    int i, reask = 0, ret = EXIT_SUCCESS;
    time_t now = time(0);

    PaStream *pa_stream = NULL;
    ring_buf_t fifo;
    float audio_gain = 1;

    std::vector<int16_t> data;

    std::string resource_filename = "res/common.res";
    std::string model_filename = "res/alexa.umdl";
    std::string sensitivity_str = "0.5";
    snowboy::SnowboyDetect detector(resource_filename, model_filename);

    if (argc == 1)
    {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    for (i = 1; i < argc; i++)
    {
        if ((!strcmp(argv[i], "-c") || !strcmp(argv[i], "--config")) && i + 1 < argc)
        {
            conf_file = argv[++i];
        }
        /*else if ((!strcmp(argv[i], "-m") || !strcmp(argv[i], "--model")) && i + 1 < argc)
        {
            model_input = argv[++i];
        }
        else if ((!strcmp(argv[i], "-r") || !strcmp(argv[i], "--res")) && i + 1 < argc)
        {
            res_input = argv[++i];
        }*/
        else if ((!strcmp(argv[i], "-s") || !strcmp(argv[i], "--sound")) && i + 1 < argc)
        {
        	listen_sound = argv[++i];
        }
        else if ((!strcmp(argv[i], "-l") || !strcmp(argv[i], "--lost")) && i + 1 < argc)
        {
            lost_sound = argv[++i];
        }
        else if ((!strcmp(argv[i], "-o") || !strcmp(argv[i], "--output")) && i + 1 < argc)
        {
            audio_output = argv[++i];
        }
        else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose"))
        {
            debug = 1;
        }
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help"))
        {
            usage(argv[0]);
            return EXIT_SUCCESS;
        }
    }

    if (conf_file == NULL)
    {
        printf("Config file did not passed\n");
        return EXIT_FAILURE;
    }
    if (-1 == access(conf_file, F_OK))
    {
        printf("Cannot access to %s\n", conf_file);
        return EXIT_FAILURE;
    }

    /*if (model_input == NULL)
    {
        printf("Model file did not passed\n");
        return EXIT_FAILURE;
    }
    if (-1 == access(model_input, F_OK))
    {
        printf("Cannot access to %s\n", model_input);
        return EXIT_FAILURE;
    }

    if (res_input == NULL)
    {
        printf("Resource file did not passed\n");
        return EXIT_FAILURE;
    }
    if (-1 == access(res_input, F_OK))
    {
        printf("Cannot access to %s\n", res_input);
        return EXIT_FAILURE;
    }*/

    if (listen_sound != NULL)
	{
        if (-1 == access(listen_sound, F_OK))
        {
            printf("Cannot access to %s\n", listen_sound);
            return EXIT_FAILURE;
        }
	    else
	    {
	        FILE *fp = fopen(listen_sound, "rb");
	        uint8_t add_size;

	        fseek(fp, 0, SEEK_END);
	        sound_size = ftell(fp);
	        fseek(fp, 0, SEEK_SET);
	        if (wav_file_read(fp, &sound_size))
	        {
	            fclose(fp);
	            ret = EXIT_FAILURE;
	            goto __EXIT;
	        }

	        add_size = (sound_size % (BYTES_PER_SAMPLE * NUMBER_OF_CHANNEL));
	        if (add_size > 0)
	            add_size = (BYTES_PER_SAMPLE * NUMBER_OF_CHANNEL) - add_size;
	        listen_sound = (char *)calloc(sound_size + add_size, sizeof(char));
	        fread(listen_sound, 1, sound_size, fp);
	        fclose(fp);

	        sound_size += add_size;
	    }
	}

    if (lost_sound != NULL)
    {
        if (-1 == access(lost_sound, F_OK))
        {
            printf("Cannot access to %s\n", lost_sound);
            return EXIT_FAILURE;
        }
        else
        {
            FILE *fp = fopen(lost_sound, "rb");
            uint8_t add_size;

            fseek(fp, 0, SEEK_END);
            lost_size = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            if (wav_file_read(fp, &lost_size))
            {
                fclose(fp);
                ret = EXIT_FAILURE;
                goto __EXIT;
            }

            add_size = (lost_size % (BYTES_PER_SAMPLE * NUMBER_OF_CHANNEL));
            if (add_size > 0)
                add_size = (BYTES_PER_SAMPLE * NUMBER_OF_CHANNEL) - add_size;
            lost_sound = (char *)calloc(lost_size + add_size, sizeof(char));
            fread(lost_sound, 1, lost_size, fp);
            fclose(fp);

            lost_size += add_size;
        }
    }

    pthread_mutex_init(&in_ring_mutex, NULL);
    pthread_mutex_init(&out_ring_mutex, NULL);
    pthread_mutex_init(&wav_mutex, NULL);
    pthread_cond_init (&wav_cond, NULL);

    cfg = ConfigNew();
    if (ConfigReadFile(conf_file, &cfg) != CONFIG_OK)
    {
        fprintf(stderr, "Read failed from file\n");
        ret = EXIT_FAILURE;
        goto __EXIT;
    }
    if (load_config(cfg, conf_file, &config, &now))
    {
        ret = EXIT_FAILURE;
        goto __FREE;
    }

    detector.SetSensitivity(sensitivity_str);
    detector.SetAudioGain(audio_gain);

    if (stream_init(&pa_stream, &fifo))
    {
        ret = EXIT_FAILURE;
        goto __FREE;
    }

    printf("Listening... Press Ctrl+C to exit\n");
    is_in = REAL_TIME_INPUT;

    while (1)
    {
        ring_buffer_size_t sz = stream_read(&(fifo.pa_input_ring_buf), &data);
        if (sz > 0)
        {
            int result = detector.RunDetection(data.data(), data.size());
            if (result > 0 || reask > 0)
            {
                printf("Hot word %d detected!\n", result);
                if (sound_size > 0)
                {
                    ring_buffer_size_t available_samples;
                    while (1)
                    {
                        available_samples = PaUtil_GetRingBufferWriteAvailable(&(fifo.pa_output_ring_buf));
                        if (available_samples >= sound_size / (BYTES_PER_SAMPLE * NUMBER_OF_CHANNEL))
                        {
                            break;
                        }
                        Pa_Sleep(10);
                    }

                    pthread_mutex_lock(&out_ring_mutex);
                    PaUtil_WriteRingBuffer(&(fifo.pa_output_ring_buf),
                            listen_sound, sound_size / (BYTES_PER_SAMPLE * NUMBER_OF_CHANNEL));
                    pthread_mutex_unlock(&out_ring_mutex);
                }

                if (!strlen(config.access_token) || config.created_time == 0 || config.expired_in == 0 ||
                        (now - config.created_time) > (config.expired_in - 120))
                {
                    config.access_token[0] = '\0';
                    if (get_access_token(config.client_id, config.client_secret, config.refresh_token,
                            config.access_token, &config.expired_in) || !strlen(config.access_token))
                    {
                        printf("Cannot get access token\n");
                        pthread_mutex_lock(&in_ring_mutex);
                        PaUtil_FlushRingBuffer(&(fifo.pa_input_ring_buf));
                        pthread_mutex_unlock(&in_ring_mutex);
                        continue;
                    }
                    else
                    {
                        config.created_time = now;
                        ConfigAddString(cfg, ALEXA_SECTION, ALEXA_ACCESS_TOKEN, config.access_token);
                        ConfigAddUnsignedInt(cfg, ALEXA_SECTION, ALEXA_CREATED_TIME, config.created_time);
                        ConfigAddUnsignedInt(cfg, ALEXA_SECTION, ALEXA_EXPIRED_IN, config.expired_in);
                        ConfigPrintToFile(cfg, conf_file);
                    }
                }

                if (strlen(config.access_token) > 0 && config.created_time > 0 && config.expired_in > 0 &&
                        (now - config.created_time) < (config.expired_in - 120))
                {
                    int length, res;
                    char *ptr = NULL;

                    printf("Please ask something!\n");
                    res = speech_request(config.access_token, &ptr, &length, &(fifo.pa_input_ring_buf));
                    if (!res && length)
                    {
                        char bond[64];
                        char *begin = NULL, *end = NULL, *tmp = NULL;

                        sscanf(ptr, "%s", bond);
                        begin = ptr + strlen(bond);

                        if ((tmp = strnstr(begin, "application/json", length - (begin - ptr))) &&
                            (end = strnstr(tmp, bond, length - (tmp - ptr))))
                        {
                            if (debug) printf("Result: %.*s\n", (int)((end - 2) - (tmp + 20)), tmp + 20);
                            reask = is_reask(tmp + 20, (end - 2) - (tmp + 20));
                            begin = end + strlen(bond);
                        }

                        while ((tmp = strnstr(begin, "audio/mpeg", length - (begin - ptr))) &&
                               (end = strnstr(tmp, bond, length - (begin - ptr))))
                        {
                            stream_write(audio_output, &(fifo.pa_output_ring_buf), tmp + 14, (end - 2) - (tmp + 14));
                            begin = end + strlen(bond);
                        }
                        free(ptr);
                    }
                    else if (res > 0 && lost_size > 0)
                    {
                        ring_buffer_size_t available_samples;
                        while (1)
                        {
                            available_samples = PaUtil_GetRingBufferWriteAvailable(&(fifo.pa_output_ring_buf));
                            if (available_samples >= lost_size / (BYTES_PER_SAMPLE * NUMBER_OF_CHANNEL))
                            {
                                break;
                            }
                            Pa_Sleep(10);
                        }

                        pthread_mutex_lock(&out_ring_mutex);
                        PaUtil_WriteRingBuffer(&(fifo.pa_output_ring_buf),
                                lost_sound, lost_size / (BYTES_PER_SAMPLE * NUMBER_OF_CHANNEL));
                        pthread_mutex_unlock(&out_ring_mutex);
                        reask = 0;
                    }
                    else
                    {
                        reask = 0;
                    }

                    is_in = REAL_TIME_INPUT;
                }
                else
                {
                    fprintf(stderr, "Something wrong with access token, created time or expired time\n");
                }
            }
        }
    }

    if (lost_size > 0) free(lost_sound);
    if (sound_size > 0) free(listen_sound);
    stream_close(pa_stream, &fifo);
    pthread_mutex_destroy(&in_ring_mutex);
    pthread_mutex_destroy(&out_ring_mutex);
    pthread_mutex_destroy(&wav_mutex);
    pthread_cond_destroy (&wav_cond);

__FREE:
    ConfigFree(cfg);
__EXIT:
    return ret;
}
