/*
 * Copyright (c) 2016 Trung Huynh
 * All rights reserved
 */
 
#ifndef __CLOUDUPLOADER_H__
#define __CLOUDUPLOADER_H__

#include <stdint.h>
#include <pa_ringbuffer.h>

#define MAXBUF      1024

#define ALEXA_SECTION          "alexa"
#define ALEXA_CLIENT_ID        "client_id"
#define ALEXA_CLIENT_SECRET    "client_secret"
#define ALEXA_REFRESH_TOKEN    "refresh_token"
#define ALEXA_ACCESS_TOKEN     "access_token"
#define ALEXA_CREATED_TIME     "created_time"
#define ALEXA_EXPIRED_IN       "expired_in"

enum input_state {
    STOP_INPUT = 0,
    RECORD_INPUT,
    REAL_TIME_INPUT
};

typedef struct data_stream {
    PaUtilRingBuffer *pa_ring_buf;
    size_t sizeleft;
}data_stream_t;

typedef struct string {
    char *ptr;
    size_t len;
}string_t;

typedef struct alexa_config
{
    char client_id[MAXBUF];
    char client_secret[MAXBUF];
    char refresh_token[MAXBUF];
    char access_token[MAXBUF];
    unsigned int created_time;
    int expired_in;

}alexa_config_t;

typedef struct ring_buf
{
    PaUtilRingBuffer pa_input_ring_buf;
    PaUtilRingBuffer pa_output_ring_buf;
    char *in_ring_buf;
    char *out_ring_buf;
}ring_buf_t;

#endif // __CLOUDUPLOADER_H__
