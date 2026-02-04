#include "streams.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Globals */
stream_t streams[MAX_STREAMS];
int stream_screen_width = 0;
int stream_screen_height = 0;

/* Private state */
static int *column_available = NULL;
static int free_slots[MAX_STREAMS];
static int free_slot_count = 0;

/* Check if a column is free with sufficient gap from neighbors */
static int column_is_spaced(int col) {
    if (!column_available[col]) return 0;
    for (int g = 1; g <= COLUMN_GAP; g++) {
        if (col - g >= 0 && !column_available[col - g]) return 0;
        if (col + g < stream_screen_width && !column_available[col + g]) return 0;
    }
    return 1;
}

/* Find a free column */
static int find_free_column(int zone) {
    (void)zone;
    if (stream_screen_width < 3) return -1;

    for (int attempt = 0; attempt < COLUMN_SEARCH_ATTEMPTS; attempt++) {
        int col = rand() % stream_screen_width;
        if (column_is_spaced(col)) {
            return col;
        }
    }

    for (int i = 0; i < stream_screen_width; i++) {
        if (column_is_spaced(i)) {
            return i;
        }
    }

    return -1;
}

/* Assign a packet to a new stream */
static void assign_packet_to_stream(packet_t *pkt) {
    if (free_slot_count == 0) return;

    int col = find_free_column(pkt->column_zone);
    if (col < 0) return;

    int idx = free_slots[--free_slot_count];
    stream_t *s = &streams[idx];

    s->state = STREAM_ACTIVE;
    s->column = col;
    s->row = 0;
    s->speed = STREAM_SPEED_MIN + (rand() % (int)(STREAM_SPEED_RANGE * 100)) / 100.0f;
    memcpy(s->text, pkt->text, pkt->length + 1);
    memcpy(s->colors, pkt->colors, pkt->length * sizeof(int));
    s->text_len = pkt->length;
    s->chars_shown = 0;
    s->frames_alive = 0;
    s->fade_at_frame = FADE_DELAY_MIN + (rand() % FADE_DELAY_RANGE);

    column_available[col] = 0;
}

/* Initialize streams for a given screen width */
void init_streams(int width) {
    stream_screen_width = width;
    memset(streams, 0, sizeof(streams));

    free_slot_count = MAX_STREAMS;
    for (int i = 0; i < MAX_STREAMS; i++) {
        free_slots[i] = MAX_STREAMS - 1 - i;
    }

    free(column_available);
    column_available = calloc(width, sizeof(int));
    if (!column_available) {
        fprintf(stderr, "Failed to allocate column_available\n");
        exit(1);
    }
    for (int i = 0; i < width; i++) {
        column_available[i] = 1;
    }
}

/* Resize streams to new dimensions */
void resize_streams(int new_width, int new_height) {
    stream_screen_width = new_width;
    stream_screen_height = new_height;

    free(column_available);
    column_available = calloc(new_width, sizeof(int));
    if (!column_available) {
        fprintf(stderr, "Failed to allocate column_available on resize\n");
        exit(1);
    }
    for (int i = 0; i < new_width; i++) {
        column_available[i] = 1;
    }

    /* Reset all streams on resize */
    memset(streams, 0, sizeof(streams));
    free_slot_count = MAX_STREAMS;
    for (int i = 0; i < MAX_STREAMS; i++) {
        free_slots[i] = MAX_STREAMS - 1 - i;
    }
}

/* Update all streams */
void update_streams(int screen_height, unsigned long frame_count) {
    packet_t pkt;
    int packets_this_frame = 0;

    while (ring_buffer_pop(&ring_buffer, &pkt) == 0 && packets_this_frame < PACKETS_PER_FRAME) {
        assign_packet_to_stream(&pkt);
        packets_this_frame++;
    }

    for (int i = 0; i < MAX_STREAMS; i++) {
        stream_t *s = &streams[i];

        if (s->state == STREAM_EMPTY) continue;

        if (s->state == STREAM_ACTIVE) {
            s->row += s->speed;
            s->frames_alive++;

            int effective_len = s->text_len < MAX_STREAM_LENGTH ? s->text_len : MAX_STREAM_LENGTH;
            int new_chars_shown = (int)s->row;
            if (new_chars_shown > effective_len) {
                new_chars_shown = effective_len;
            }

            s->chars_shown = new_chars_shown;

            int tail_row = (int)s->row - s->chars_shown;
            if (tail_row > screen_height || s->frames_alive >= s->fade_at_frame) {
                s->state = STREAM_FADING;
                if (s->row >= screen_height) {
                    s->chars_shown -= (int)s->row - (screen_height - 1);
                    s->row = screen_height - 1;
                    if (s->chars_shown < 1) s->chars_shown = 1;
                }
            }
        } else if (s->state == STREAM_FADING) {
            s->chars_shown -= FADE_RATE;
            if (s->chars_shown <= 0) {
                column_available[s->column] = 1;
                s->state = STREAM_EMPTY;
                if (free_slot_count < MAX_STREAMS) {
                    free_slots[free_slot_count++] = i;
                }
            }
        }
    }

    update_network_rate(frame_count);
}

/* Returns 1 if any stream is active or fading */
int streams_have_content(void) {
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (streams[i].state != STREAM_EMPTY)
            return 1;
    }
    return 0;
}
