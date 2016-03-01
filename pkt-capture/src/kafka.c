/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "kafka.h"

#define POLL_TIMEOUT_MS 1000

/*
 * data structures required for the kafka client
 */
static rd_kafka_t **kaf_h;
static rd_kafka_topic_t **kaf_top_h;
static int num_conns;
static const char *topic_nm = "pcap";

/**
 * Initializes a pool of Kafka connections.
 */
void kaf_init(int num_of_conns)
{
  int i;
  int size = 512;
  char errstr[512];
  rd_kafka_conf_res_t rc;

  // the number of connections to maintain
  num_conns = num_of_conns;

  // create kafka resources for each consumer
  kaf_h = calloc(num_of_conns, sizeof(rd_kafka_t *));
  kaf_top_h = calloc(num_of_conns, sizeof(rd_kafka_topic_t *));

  for (i = 0; i < num_of_conns; i++)
  {
    // configure kafka global settings
    rd_kafka_conf_t *kaf_conf = rd_kafka_conf_new();
    rc = rd_kafka_conf_set(kaf_conf, "metadata.broker.list", "localhost:9092", errstr, size);
    if (RD_KAFKA_CONF_OK != rc) {
      rte_exit(EXIT_FAILURE, "Unable to set kafka broker: %s", errstr);
    }

    // create a new kafka handle
    kaf_h[i] = rd_kafka_new(RD_KAFKA_PRODUCER, kaf_conf, errstr, sizeof(errstr));
    if (!kaf_h[i]) {
      rte_exit(EXIT_FAILURE, "Cannot init kafka connection: %s", errstr);
    }

    // configure kafka topic settings
    rd_kafka_topic_conf_t *topic_conf = rd_kafka_topic_conf_new();
    kaf_top_h[i] = rd_kafka_topic_new(kaf_h[i], topic_nm, topic_conf);
    if (!kaf_top_h[i])
    {
      rte_exit(EXIT_FAILURE, "Cannot init kafka topic: %s", topic_nm);
    }
  }
}

/**
 * Closes the pool of Kafka connections.
 */
void kaf_close(void)
{
  int i;

  for (i = 0; i < num_conns; i++)
  {
    // wait for messages to be delivered
    while (rd_kafka_outq_len(kaf_h[i]) > 0)
    {
      printf("waiting for %d messages to clear on conn [%i/%i]",
              rd_kafka_outq_len(kaf_h[i]),
              i+1,
              num_conns);
      rd_kafka_poll(kaf_h[i], POLL_TIMEOUT_MS);
    }

    rd_kafka_topic_destroy(kaf_top_h[i]);
    rd_kafka_destroy(kaf_h[i]);
  }
}

/**
 * The current time in microseconds.
 */
static uint64_t current_time(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * (uint64_t) 1000000 + tv.tv_usec;
}

/**
 * Publish a set of packets to a kafka topic.
 */
int kaf_send(struct rte_mbuf *data, int pkt_count, int conn_id)
{
  int i;
  int partition = RD_KAFKA_PARTITION_UA; // unassigned partition
  int pkts_sent = 0;
  int drops;
  rd_kafka_message_t kaf_msgs[pkt_count];
  char key_buf[32];

  // stamp each message with os clock time in lieu of an accurate
  // timestamp from the network device
  sprintf(key_buf, "%"PRIu64"", current_time());

  // find the topic connection based on the conn_id
  rd_kafka_topic_t *kaf_topic = kaf_top_h[conn_id];

  // create the batch message for kafka
  for (i = 0; i < pkt_count; i++)
  {
    kaf_msgs[i].err = 0;
    kaf_msgs[i].rkt = kaf_topic;
    kaf_msgs[i].partition = RD_KAFKA_PARTITION_UA;
    kaf_msgs[i].payload = rte_ctrlmbuf_data(&data[i]);
    kaf_msgs[i].len = rte_ctrlmbuf_len(&data[i]);
    kaf_msgs[i].key = key_buf;
    kaf_msgs[i].key_len = sizeof(key_buf);
    kaf_msgs[i].offset = 0;
  }

  // hand all of the messages off to kafka
  pkts_sent = rd_kafka_produce_batch(kaf_topic, partition, 0, kaf_msgs, pkt_count);

  // did we drop packets?
  drops = pkt_count - pkts_sent;
  if(drops > 0)
  {
    for(i = 0; i < pkt_count; i++)
    {
      if(!kaf_msgs[i].err)
      {
        printf("'%d' packets dropped, first error: %s", drops, (char*) kaf_msgs[i].payload);
      }
    }
  }

  return pkts_sent;
}
