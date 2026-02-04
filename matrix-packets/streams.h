#ifndef STREAMS_H
#define STREAMS_H

#include "capture.h"

/* Configuration */
#define MAX_STREAMS        512
#define MAX_STREAM_LENGTH  160
#define STREAM_SPEED_MIN   0.4f
#define STREAM_SPEED_RANGE 1.5f
#define FADE_DELAY_MIN     30
#define FADE_DELAY_RANGE   120
#define FADE_RATE          2
#define TRAIL_DIM_DISTANCE 15
#define BLINK_CYCLE        6
#define BLINK_ON           3
#define COLUMN_GAP         1
#define PACKETS_PER_FRAME  20
#define COLUMN_SEARCH_ATTEMPTS 40

/* Stream states */
#define STREAM_EMPTY     0
#define STREAM_ACTIVE    1
#define STREAM_FADING    2

typedef struct {
    int state;
    int column;
    float row;
    float speed;
    char text[MAX_INFO_LEN];
    int colors[MAX_INFO_LEN];
    int text_len;
    int chars_shown;
    int frames_alive;
    int fade_at_frame;
} stream_t;

/* Globals (defined in streams.c) */
extern stream_t streams[MAX_STREAMS];
extern int stream_screen_width;
extern int stream_screen_height;

/* Initialize streams for a given screen width */
void init_streams(int width);

/* Resize streams to new dimensions */
void resize_streams(int new_width, int new_height);

/* Update all streams (pop from ring buffer, advance positions) */
void update_streams(int screen_height, unsigned long frame_count);

/* Returns 1 if any stream is active or fading */
int streams_have_content(void);

#endif /* STREAMS_H */
