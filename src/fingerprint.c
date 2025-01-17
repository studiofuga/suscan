/*

  Copyright (C) 2017 Gonzalo José Carracedo Carballal

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation, version 3.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this program.  If not, see
  <http://www.gnu.org/licenses/>

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <getopt.h>
#include <sys/time.h>
#include <sys/types.h>
#include <util/compat-select.h>

#define SU_LOG_DOMAIN "fingerprint"

#include "suscan.h"

#define SUSCAN_CHLIST_SKIP_CHANNELS 50
#define SUSCAN_BRINSP_SKIP_CHANNELS 50

struct suscan_fingerprint_chresult {
  struct sigutils_channel channel;
  SUHANDLE br_handle; /* Baudrate inspector handle */
};

struct suscan_fingerprint_report {
  struct suscan_fingerprint_chresult *results;
  unsigned int result_count;
};

void
suscan_fingerprint_report_destroy(
    struct suscan_fingerprint_report *report)
{
  if (report->results != NULL)
    free(report->results);

  free(report);
}

struct suscan_fingerprint_report *
suscan_fingerprint_report_new(
    struct sigutils_channel **list,
    unsigned int count)
{
  struct suscan_fingerprint_report *new = NULL;
  unsigned int i;

  if ((new = malloc(sizeof (struct suscan_fingerprint_report))) == NULL)
    goto fail;

  new->result_count = count;

  if ((new->results =
      calloc(count, sizeof(struct suscan_fingerprint_chresult))) == NULL)
    goto fail;

  for (i = 0; i < count; ++i) {
    new->results[i].channel = *(list[i]);
    new->results[i].br_handle = -1;
  }

  return new;

fail:
  if (new != NULL)
    suscan_fingerprint_report_destroy(new);

  return NULL;
}

SUBOOL
suscan_open_all_channels(
    suscan_analyzer_t *analyzer,
    struct suscan_fingerprint_report *report)
{
  unsigned int i;
  SUHANDLE handle;

  for (i = 0; i < report->result_count; ++i) {
    handle = suscan_analyzer_open(
        analyzer,
        "psk",
        &report->results[i].channel);
    if (handle == -1) {
      SU_ERROR("Failed to open baud inspector\n");
      return SU_FALSE;
    }

    report->results[i].br_handle = handle;
  }

  return SU_TRUE;
}

void
suscan_close_all_channels(
    suscan_analyzer_t *analyzer,
    struct suscan_fingerprint_report *report)
{
  unsigned int i;

  for (i = 0; i < report->result_count; ++i)
    if (report->results[i].br_handle >= 0)
      (void) suscan_analyzer_close(
          analyzer,
          report->results[i].br_handle);
}

SUBOOL
suscan_get_all_baudrates(
    suscan_analyzer_t *analyzer,
    struct suscan_fingerprint_report *report)
{
  unsigned int i;

  for (i = 0; i < report->result_count; ++i) {
    /* TODO: Implement */
#if 0
    if (!suscan_analyzer_get_info(
        analyzer,
        report->results[i].br_handle,
        &report->results[i].baudrate)) {
      SU_ERROR("Failed to get baudrate for channel #%d\n", i + 1);
      return SU_FALSE;
    }
#endif
  }

  return SU_TRUE;
}

void
suscan_print_report(
    const struct suscan_fingerprint_report *report)
{
  unsigned int i;

  printf(" id |   Channel freq.  |  Bandwidth (hi - lo) |    SNR   | Baud (a) | Baud (n)\n");
  printf("----+------------------+----------------------+----------+----------+-----------\n");

  for (i = 0; i < report->result_count; ++i)
    printf(
        "%2u. | %+8.1lf Hz | %7.1lf (%7.1lf) Hz | %5.1lf dB | %8s | %8s \n",
        i + 1,
        report->results[i].channel.fc,
        report->results[i].channel.bw,
        report->results[i].channel.f_hi - report->results[i].channel.f_lo,
        report->results[i].channel.snr,
        "N/A",
        "N/A");
}

SUBOOL
suscan_perform_fingerprint(struct suscan_source_config *config)
{
  struct suscan_mq mq;
  void *private;
  uint32_t type;
  suscan_analyzer_t *analyzer = NULL;
  struct suscan_analyzer_params params = suscan_analyzer_params_INITIALIZER;
  const struct suscan_analyzer_channel_msg *ch_msg;
  const struct suscan_analyzer_status_msg  *st_msg;
  struct suscan_fingerprint_report *report = NULL;
  unsigned int chskip = SUSCAN_CHLIST_SKIP_CHANNELS;
  SUBOOL running = SU_TRUE;
  SUBOOL ok = SU_FALSE;

  if (!suscan_mq_init(&mq))
    return SU_FALSE;

  SU_TRYCATCH(analyzer = suscan_analyzer_new(&params, config, &mq), goto done);

  while (running) {
    private = suscan_analyzer_read(analyzer, &type);

    switch (type) {
      case SUSCAN_ANALYZER_MESSAGE_TYPE_CHANNEL:
        ch_msg = (struct suscan_analyzer_channel_msg *) private;
        if (chskip > 0) {
          --chskip;
        } else if (report == NULL) {
          suscan_channel_list_sort(ch_msg->channel_list, ch_msg->channel_count);
          if ((report = suscan_fingerprint_report_new(
              ch_msg->channel_list,
              ch_msg->channel_count)) == NULL) {
            SU_ERROR("Failed to create report\n");
            running = SU_FALSE;
          } else if (!suscan_open_all_channels(analyzer, report)) {
            SU_ERROR("Failed to open all channels\n");
            running = SU_FALSE;
          } else {
            chskip = SUSCAN_BRINSP_SKIP_CHANNELS;
            SU_INFO(
                "Found %d channels, wait for %d channel updates\n",
                report->result_count,
                chskip);
          }
        } else {
          if (!suscan_get_all_baudrates(analyzer, report)) {
            SU_ERROR("Failed to get all baudrates\n");
          } else {
            suscan_print_report(report);
          }

          running = SU_FALSE;
        }

        break;

      case SUSCAN_ANALYZER_MESSAGE_TYPE_EOS:
        st_msg = (struct suscan_analyzer_status_msg *) private;

        if (st_msg->err_msg != NULL)
          SU_WARNING("End of stream: %s\n", st_msg->err_msg);
        else
          SU_WARNING("Unexpected end of stream\n");

        running = SU_FALSE;
        break;
    }

    suscan_analyzer_dispose_message(type, private);
  }

  ok = SU_TRUE;

done:
  if (report != NULL) {
    suscan_close_all_channels(analyzer, report);
    suscan_fingerprint_report_destroy(report);
  }

  if (analyzer != NULL)
    suscan_analyzer_destroy(analyzer);

  suscan_analyzer_consume_mq(&mq);
  suscan_mq_finalize(&mq);

  return ok;
}
