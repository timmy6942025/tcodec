/*
 * threadpool.c — Thread pool for WPP parallelism in TCodec
 *
 * Wavefront Parallel Processing: CTU rows can be processed in
 * parallel once the row above has completed its first 2 CTUs
 * (providing reference samples for intra prediction).
 *
 * This thread pool manages worker threads that process rows
 * in a wavefront pattern with minimal synchronization.
 *
 * Entire file is guarded by !TCODEC_NO_THREADS — when threading
 * is disabled, this file compiles to nothing.
 */

#include "tcodec_common.h"

#if !defined(TCODEC_NO_THREADS)

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ── Worker thread entry point ───────────────────────────────── */

static void *worker_func(void *arg)
{
    tc_threadpool_t *pool = (tc_threadpool_t *)arg;

    while (1) {
        pthread_mutex_lock(&pool->mutex);

        /* Wait for work or shutdown */
        while (pool->next_row >= pool->total_rows && !pool->shutdown) {
            pthread_cond_wait(&pool->work_cond, &pool->mutex);
        }

        if (pool->shutdown) {
            pthread_mutex_unlock(&pool->mutex);
            return NULL;
        }

        /* Get next row to process */
        int row = pool->next_row;
        if (row >= pool->total_rows) {
            pthread_mutex_unlock(&pool->mutex);
            continue;
        }
        pool->next_row++;

        /* WPP dependency: row N can start after row N-1 has
         * completed at least 2 CTUs. For simplicity, we enforce
         * that row N waits until row N-1 is fully done.
         * A more sophisticated implementation would track
         * per-CTU completion. */
        int wait_row = row - 1;
        if (wait_row >= 0 && !pool->row_done[wait_row]) {
            /* Wait for previous row to complete */
            while (!pool->row_done[wait_row]) {
                pthread_cond_wait(&pool->done_cond, &pool->mutex);
            }
        }

        pthread_mutex_unlock(&pool->mutex);

        /* Do the actual work */
        pool->func(pool->ctx, row);

        /* Mark this row as done */
        pthread_mutex_lock(&pool->mutex);
        pool->row_done[row] = 1;
        /* Wake up any threads waiting for this row */
        pthread_cond_broadcast(&pool->done_cond);
        pthread_mutex_unlock(&pool->mutex);
    }
}

/* ── Pool lifecycle ──────────────────────────────────────────── */

tc_threadpool_t *tc_threadpool_create(int num_threads)
{
    if (num_threads < 1) num_threads = 1;

    tc_threadpool_t *pool = (tc_threadpool_t *)calloc(1, sizeof(tc_threadpool_t));
    if (!pool) return NULL;

    pool->num_threads = num_threads;
    pool->threads = (pthread_t *)calloc((size_t)num_threads, sizeof(pthread_t));
    if (!pool->threads) { free(pool); return NULL; }

    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->work_cond, NULL);
    pthread_cond_init(&pool->done_cond, NULL);
    pool->shutdown = 0;

    /* Start worker threads */
    for (int i = 0; i < num_threads; i++) {
        pthread_create(&pool->threads[i], NULL, worker_func, pool);
    }

    return pool;
}

void tc_threadpool_destroy(tc_threadpool_t *pool)
{
    if (!pool) return;

    pthread_mutex_lock(&pool->mutex);
    pool->shutdown = 1;
    pthread_cond_broadcast(&pool->work_cond);
    pthread_mutex_unlock(&pool->mutex);

    for (int i = 0; i < pool->num_threads; i++) {
        pthread_join(pool->threads[i], NULL);
    }

    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->work_cond);
    pthread_cond_destroy(&pool->done_cond);

    free(pool->threads);
    free(pool->row_done);
    free(pool);
}

void tc_threadpool_run(tc_threadpool_t *pool,
                        tc_wpp_row_func func, void *ctx,
                        int total_rows)
{
    /* Allocate row completion flags */
    pool->row_done = (int *)calloc((size_t)total_rows, sizeof(int));
    pool->func     = func;
    pool->ctx      = ctx;
    pool->next_row = 0;
    pool->total_rows = total_rows;

    /* Wake all workers */
    pthread_mutex_lock(&pool->mutex);
    pthread_cond_broadcast(&pool->work_cond);
    pthread_mutex_unlock(&pool->mutex);

    /* Wait for all rows to complete */
    pthread_mutex_lock(&pool->mutex);
    while (1) {
        int all_done = 1;
        for (int i = 0; i < total_rows; i++) {
            if (!pool->row_done[i]) { all_done = 0; break; }
        }
        if (all_done) break;
        pthread_cond_wait(&pool->done_cond, &pool->mutex);
    }
    pthread_mutex_unlock(&pool->mutex);

    free(pool->row_done);
    pool->row_done = NULL;
}

#endif /* TCODEC_NO_THREADS */
