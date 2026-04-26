#ifndef STATS_H
#define STATS_H

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "document_layout.h"

typedef struct
{
    SemaphoreHandle_t state_mutex;
    uint32_t *frame_count;
    uint32_t *last_frame_count;
    const document_layout_t *doc_layout;
    uint32_t (*count_loaded_tiles_fn)(void);
} stats_context_t;

void stats_task(void *arg);

#endif
