/*
    pmacct (Promiscuous mode IP Accounting package)
    pmacct is Copyright (C) 2003-2017 by Paolo Lucente
*/

/*
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

#define __KAFKA_PLUGIN_C

/* includes */
#include "pmacct.h"
#include "pmacct-data.h"
#include "plugin_hooks.h"
#include "plugin_common.h"
#include "kafka_plugin.h"
#ifndef WITH_JANSSON
#error "--enable-kafka requires --enable-jansson"
#endif
#ifdef WITH_AVRO
#include <avro.h>
#endif

/* Functions */
void kafka_plugin(int pipe_fd, struct configuration *cfgptr, void *ptr)
{
  struct pkt_data *data;
  struct ports_table pt;
  unsigned char *pipebuf;
  struct pollfd pfd;
  struct insert_data idata;
  time_t t, avro_schema_deadline = 0;
  int timeout, refresh_timeout, amqp_timeout = 0, avro_schema_timeout = 0;
  int ret, num; 
  struct ring *rg = &((struct channels_list_entry *)ptr)->rg;
  struct ch_status *status = ((struct channels_list_entry *)ptr)->status;
  struct plugins_list_entry *plugin_data = ((struct channels_list_entry *)ptr)->plugin;
  int datasize = ((struct channels_list_entry *)ptr)->datasize;
  u_int32_t bufsz = ((struct channels_list_entry *)ptr)->bufsize;
  pid_t core_pid = ((struct channels_list_entry *)ptr)->core_pid;
  struct networks_file_data nfd;

  unsigned char *rgptr;
  int pollagain = TRUE;
  u_int32_t seq = 1, rg_err_count = 0;

  struct extra_primitives extras;
  struct primitives_ptrs prim_ptrs;
  char *dataptr;

#ifdef WITH_RABBITMQ
  struct p_amqp_host *amqp_host = &((struct channels_list_entry *)ptr)->amqp_host;
#endif

  memcpy(&config, cfgptr, sizeof(struct configuration));
  memcpy(&extras, &((struct channels_list_entry *)ptr)->extras, sizeof(struct extra_primitives));
  recollect_pipe_memory(ptr);
  pm_setproctitle("%s [%s]", "Kafka Plugin", config.name);

  P_set_signals();
  P_init_default_values();
  P_config_checks();
  pipebuf = (unsigned char *) pm_malloc(config.buffer_size);
  memset(pipebuf, 0, config.buffer_size);

  timeout = config.sql_refresh_time*1000;

  if (!config.message_broker_output) config.message_broker_output = PRINT_OUTPUT_JSON;

  if (config.message_broker_output & PRINT_OUTPUT_AVRO) {
#ifdef WITH_AVRO
    Log(LOG_INFO, "INFO ( %s/%s ): AVRO: building schema.\n", config.name, config.type);
    avro_acct_schema = build_avro_schema(config.what_to_count, config.what_to_count_2);

    if (config.avro_schema_output_file) {
      FILE *avro_fp = open_output_file(config.avro_schema_output_file, "w", TRUE);
      avro_writer_t avro_schema_writer = avro_writer_file(avro_fp);

      if (avro_schema_to_json(avro_acct_schema, avro_schema_writer)) {
        Log(LOG_ERR, "ERROR ( %s/%s ): AVRO: unable to dump schema: %s\n",
            config.name, config.type, avro_strerror());
        exit_plugin(1);
      }

      close_output_file(avro_fp);
    }

    if (config.kafka_avro_schema_topic) {
      if (!config.kafka_avro_schema_refresh_time)
	config.kafka_avro_schema_refresh_time = DEFAULT_AVRO_SCHEMA_REFRESH_TIME;

      avro_schema_deadline = time(NULL);
      P_init_refresh_deadline(&avro_schema_deadline, config.kafka_avro_schema_refresh_time, 0, "m");
    }
    else {
      config.kafka_avro_schema_refresh_time = 0;
      avro_schema_deadline = 0;
      avro_schema_timeout = 0;
    }
#endif
  }

  if ((config.sql_table && strchr(config.sql_table, '$')) && config.sql_multi_values) {
    Log(LOG_ERR, "ERROR ( %s/%s ): dynamic 'kafka_topic' is not compatible with 'kafka_multi_values'. Exiting.\n", config.name, config.type);
    exit_plugin(1);
  }

  if ((config.sql_table && strchr(config.sql_table, '$')) && config.amqp_routing_key_rr) {
    Log(LOG_ERR, "ERROR ( %s/%s ): dynamic 'kafka_topic' is not compatible with 'kafka_topic_rr'. Exiting.\n", config.name, config.type);
    exit_plugin(1);
  }

  /* setting function pointers */
  if (config.what_to_count & (COUNT_SUM_HOST|COUNT_SUM_NET))
    insert_func = P_sum_host_insert;
  else if (config.what_to_count & COUNT_SUM_PORT) insert_func = P_sum_port_insert;
  else if (config.what_to_count & COUNT_SUM_AS) insert_func = P_sum_as_insert;
#if defined (HAVE_L2)
  else if (config.what_to_count & COUNT_SUM_MAC) insert_func = P_sum_mac_insert;
#endif
  else insert_func = P_cache_insert;
  purge_func = kafka_cache_purge;

  memset(&nt, 0, sizeof(nt));
  memset(&nc, 0, sizeof(nc));
  memset(&pt, 0, sizeof(pt));

  load_networks(config.networks_file, &nt, &nc);
  set_net_funcs(&nt);

  if (config.ports_file) load_ports(config.ports_file, &pt);
  if (config.pkt_len_distrib_bins_str) load_pkt_len_distrib_bins();
  else {
    if (config.what_to_count_2 & COUNT_PKT_LEN_DISTRIB) {
      Log(LOG_ERR, "ERROR ( %s/%s ): 'aggregate' contains pkt_len_distrib but no 'pkt_len_distrib_bins' defined. Exiting.\n", config.name, config.type);
      exit_plugin(1);
    }
  }
  
  memset(&idata, 0, sizeof(idata));
  memset(&prim_ptrs, 0, sizeof(prim_ptrs));
  set_primptrs_funcs(&extras);

  if (config.pipe_amqp) {
    plugin_pipe_amqp_compile_check();
#ifdef WITH_RABBITMQ
    pipe_fd = plugin_pipe_amqp_connect_to_consume(amqp_host, plugin_data);
    amqp_timeout = plugin_pipe_set_retry_timeout(&amqp_host->btimers, pipe_fd);
#endif
  }
  else setnonblocking(pipe_fd);

  idata.now = time(NULL);

  /* print_refresh time init: deadline */
  refresh_deadline = idata.now; 
  P_init_refresh_deadline(&refresh_deadline, config.sql_refresh_time, config.sql_startup_delay, config.sql_history_roundoff);

  if (config.sql_history) {
    basetime_init = P_init_historical_acct;
    basetime_eval = P_eval_historical_acct;
    basetime_cmp = P_cmp_historical_acct;

    (*basetime_init)(idata.now);
  }

  /* setting number of entries in _protocols structure */
  while (_protocols[protocols_number].number != -1) protocols_number++;

  /* plugin main loop */
  for(;;) {
    poll_again:
    status->wakeup = TRUE;
    calc_refresh_timeout(refresh_deadline, idata.now, &refresh_timeout);
    if (config.kafka_avro_schema_topic) calc_refresh_timeout(avro_schema_deadline, idata.now, &avro_schema_timeout);

    pfd.fd = pipe_fd;
    pfd.events = POLLIN;
    timeout = MIN(refresh_timeout, (amqp_timeout ? amqp_timeout : INT_MAX));
    timeout = MIN(timeout, (avro_schema_timeout ? avro_schema_timeout : INT_MAX));
    ret = poll(&pfd, (pfd.fd == ERR ? 0 : 1), timeout);

    if (ret <= 0) {
      if (getppid() == 1) {
        Log(LOG_ERR, "ERROR ( %s/%s ): Core process *seems* gone. Exiting.\n", config.name, config.type);
        exit_plugin(1);
      }

      if (ret < 0) goto poll_again;
    }

    idata.now = time(NULL);

    if (config.sql_history) {
      while (idata.now > (basetime.tv_sec + timeslot)) {
	new_basetime.tv_sec = basetime.tv_sec;
        basetime.tv_sec += timeslot;
        if (config.sql_history == COUNT_MONTHLY)
          timeslot = calc_monthly_timeslot(basetime.tv_sec, config.sql_history_howmany, ADD);
      }
    }

#ifdef WITH_RABBITMQ
    if (config.pipe_amqp && pipe_fd == ERR) {
      if (timeout == amqp_timeout) {
        pipe_fd = plugin_pipe_amqp_connect_to_consume(amqp_host, plugin_data);
        amqp_timeout = plugin_pipe_set_retry_timeout(&amqp_host->btimers, pipe_fd);
      }
      else amqp_timeout = plugin_pipe_calc_retry_timeout_diff(&amqp_host->btimers, idata.now);
    }
#endif

#ifdef WITH_AVRO
    if (idata.now > avro_schema_deadline) {
      kafka_avro_schema_purge();
      avro_schema_deadline += config.kafka_avro_schema_refresh_time;
    }
#endif

    switch (ret) {
    case 0: /* timeout */
      if (idata.now > refresh_deadline) P_cache_handle_flush_event(&pt);
      break;
    default: /* we received data */
      read_data:
      if (!config.pipe_amqp) {
        if (!pollagain) {
          seq++;
          seq %= MAX_SEQNUM;
          if (seq == 0) rg_err_count = FALSE;
        }
        else {
          if ((ret = read(pipe_fd, &rgptr, sizeof(rgptr))) == 0) 
	    exit_plugin(1); /* we exit silently; something happened at the write end */
        }

        if ((rg->ptr + bufsz) > rg->end) rg->ptr = rg->base;

        if (((struct ch_buf_hdr *)rg->ptr)->seq != seq) {
          if (!pollagain) {
            pollagain = TRUE;
            goto poll_again;
          }
          else {
            rg_err_count++;
            if (config.debug || (rg_err_count > MAX_RG_COUNT_ERR)) {
              Log(LOG_WARNING, "WARN ( %s/%s ): Missing data detected (plugin_buffer_size=%llu plugin_pipe_size=%llu).\n",
                        config.name, config.type, config.buffer_size, config.pipe_size);
              Log(LOG_WARNING, "WARN ( %s/%s ): Increase values or look for plugin_buffer_size, plugin_pipe_size in CONFIG-KEYS document.\n\n",
                        config.name, config.type);
            }

	    rg->ptr = (rg->base + status->last_buf_off);
            seq = ((struct ch_buf_hdr *)rg->ptr)->seq;
          }
        }

        pollagain = FALSE;
        memcpy(pipebuf, rg->ptr, bufsz);
        rg->ptr += bufsz;
      }
#ifdef WITH_RABBITMQ
      else {
        ret = p_amqp_consume_binary(amqp_host, pipebuf, config.buffer_size);
        if (ret) pipe_fd = ERR;

        seq = ((struct ch_buf_hdr *)pipebuf)->seq;
        amqp_timeout = plugin_pipe_set_retry_timeout(&amqp_host->btimers, pipe_fd);
      }
#endif

      /* lazy refresh time handling */ 
      if (idata.now > refresh_deadline) P_cache_handle_flush_event(&pt);

      data = (struct pkt_data *) (pipebuf+sizeof(struct ch_buf_hdr));

      if (config.debug_internal_msg) 
        Log(LOG_DEBUG, "DEBUG ( %s/%s ): buffer received cpid=%u len=%llu seq=%u num_entries=%u\n",
                config.name, config.type, core_pid, ((struct ch_buf_hdr *)pipebuf)->len,
                seq, ((struct ch_buf_hdr *)pipebuf)->num);

      if (!config.pipe_check_core_pid || ((struct ch_buf_hdr *)pipebuf)->core_pid == core_pid) {
      while (((struct ch_buf_hdr *)pipebuf)->num > 0) {
        for (num = 0; primptrs_funcs[num]; num++)
          (*primptrs_funcs[num])((u_char *)data, &extras, &prim_ptrs);

	for (num = 0; net_funcs[num]; num++)
	  (*net_funcs[num])(&nt, &nc, &data->primitives, prim_ptrs.pbgp, &nfd);

	if (config.ports_file) {
          if (!pt.table[data->primitives.src_port]) data->primitives.src_port = 0;
          if (!pt.table[data->primitives.dst_port]) data->primitives.dst_port = 0;
        }

        if (config.pkt_len_distrib_bins_str &&
            config.what_to_count_2 & COUNT_PKT_LEN_DISTRIB)
          evaluate_pkt_len_distrib(data);

        prim_ptrs.data = data;
        (*insert_func)(&prim_ptrs, &idata);

	((struct ch_buf_hdr *)pipebuf)->num--;
        if (((struct ch_buf_hdr *)pipebuf)->num) {
          dataptr = (unsigned char *) data;
          if (!prim_ptrs.vlen_next_off) dataptr += datasize;
          else dataptr += prim_ptrs.vlen_next_off;
          data = (struct pkt_data *) dataptr;
	}
      }
      }

      if (!config.pipe_amqp) goto read_data;
    }
  }
}

void kafka_cache_purge(struct chained_cache *queue[], int index)
{
  struct pkt_primitives *data = NULL;
  struct pkt_bgp_primitives *pbgp = NULL;
  struct pkt_nat_primitives *pnat = NULL;
  struct pkt_mpls_primitives *pmpls = NULL;
  char *pcust = NULL;
  struct pkt_vlen_hdr_primitives *pvlen = NULL;
  struct pkt_bgp_primitives empty_pbgp;
  struct pkt_nat_primitives empty_pnat;
  struct pkt_mpls_primitives empty_pmpls;
  char *empty_pcust = NULL;
  char src_mac[18], dst_mac[18], src_host[INET6_ADDRSTRLEN], dst_host[INET6_ADDRSTRLEN], ip_address[INET6_ADDRSTRLEN];
  char rd_str[SRVBUFLEN], misc_str[SRVBUFLEN], dyn_kafka_topic[SRVBUFLEN], *orig_kafka_topic = NULL;
  int i, j, stop, batch_idx, is_topic_dyn = FALSE, qn = 0, ret, saved_index = index;
  int mv_num = 0, mv_num_save = 0;
  time_t start, duration;
  pid_t writer_pid = getpid();

#ifdef WITH_JANSSON
  json_t *array = json_array();
#endif

#ifdef WITH_AVRO
  avro_writer_t avro_writer;
  char *avro_buf = NULL;
  int avro_buffer_full = FALSE;
#endif

  p_kafka_init_host(&kafkap_kafka_host, config.kafka_config_file);

  /* setting some defaults */
  if (!config.sql_host) config.sql_host = default_kafka_broker_host;
  if (!config.kafka_broker_port) config.kafka_broker_port = default_kafka_broker_port;

  if (!config.sql_table) config.sql_table = default_kafka_topic;
  else {
    if (strchr(config.sql_table, '$')) {
      is_topic_dyn = TRUE;
      orig_kafka_topic = config.sql_table;
    }
  }

  if (config.amqp_routing_key_rr) orig_kafka_topic = config.sql_table;

  p_kafka_init_topic_rr(&kafkap_kafka_host);
  p_kafka_set_topic_rr(&kafkap_kafka_host, config.amqp_routing_key_rr);

  empty_pcust = malloc(config.cpptrs.len);
  if (!empty_pcust) {
    Log(LOG_ERR, "ERROR ( %s/%s ): Unable to malloc() empty_pcust. Exiting.\n", config.name, config.type);
    exit_plugin(1);
  }

  memset(&empty_pbgp, 0, sizeof(struct pkt_bgp_primitives));
  memset(&empty_pnat, 0, sizeof(struct pkt_nat_primitives));
  memset(&empty_pmpls, 0, sizeof(struct pkt_mpls_primitives));
  memset(empty_pcust, 0, config.cpptrs.len);

  p_kafka_connect_to_produce(&kafkap_kafka_host);
  p_kafka_set_broker(&kafkap_kafka_host, config.sql_host, config.kafka_broker_port);
  if (!is_topic_dyn && !config.amqp_routing_key_rr) p_kafka_set_topic(&kafkap_kafka_host, config.sql_table);
  p_kafka_set_partition(&kafkap_kafka_host, config.kafka_partition);
  p_kafka_set_key(&kafkap_kafka_host, config.kafka_partition_key, config.kafka_partition_keylen);

  if (config.message_broker_output & PRINT_OUTPUT_JSON) p_kafka_set_content_type(&kafkap_kafka_host, PM_KAFKA_CNT_TYPE_STR);
  else if (config.message_broker_output & PRINT_OUTPUT_AVRO) p_kafka_set_content_type(&kafkap_kafka_host, PM_KAFKA_CNT_TYPE_BIN);
  else {
    Log(LOG_ERR, "ERROR ( %s/%s ): Unsupported kafka_output value specified. Exiting.\n", config.name, config.type);
    exit_plugin(1);
  }

  for (j = 0, stop = 0; (!stop) && P_preprocess_funcs[j]; j++)
    stop = P_preprocess_funcs[j](queue, &index, j);

  Log(LOG_INFO, "INFO ( %s/%s ): *** Purging cache - START (PID: %u) ***\n", config.name, config.type, writer_pid);
  start = time(NULL);

  if (config.print_markers) {
    if (config.message_broker_output & PRINT_OUTPUT_JSON) {
      void *json_obj;
      char *json_str;

      json_obj = compose_purge_init_json(config.name, writer_pid);

      if (json_obj) json_str = compose_json_str(json_obj);
      if (json_str) {
        Log(LOG_DEBUG, "DEBUG ( %s/%s ): %s\n\n", config.name, config.type, json_str);
        ret = p_kafka_produce_data(&kafkap_kafka_host, json_str, strlen(json_str));

        free(json_str);
        json_str = NULL;
      }
    }
  }

#ifdef WITH_AVRO
  if (config.message_broker_output & PRINT_OUTPUT_AVRO) {
    if (!config.avro_buffer_size) config.avro_buffer_size = LARGEBUFLEN;

    avro_buf = malloc(config.avro_buffer_size);

    if (!avro_buf) {
      Log(LOG_ERR, "ERROR ( %s/%s ): malloc() failed (avro_buf). Exiting ..\n", config.name, config.type);
      exit_plugin(1);
    }

    avro_writer = avro_writer_memory(avro_buf, config.avro_buffer_size);
  }
#endif

  for (j = 0; j < index; j++) {
    void *json_obj;
    char *json_str;

    if (queue[j]->valid != PRINT_CACHE_COMMITTED) continue;

    data = &queue[j]->primitives;
    if (queue[j]->pbgp) pbgp = queue[j]->pbgp;
    else pbgp = &empty_pbgp;

    if (queue[j]->pnat) pnat = queue[j]->pnat;
    else pnat = &empty_pnat;

    if (queue[j]->pmpls) pmpls = queue[j]->pmpls;
    else pmpls = &empty_pmpls;

    if (queue[j]->pcust) pcust = queue[j]->pcust;
    else pcust = empty_pcust;

    if (queue[j]->pvlen) pvlen = queue[j]->pvlen;
    else pvlen = NULL;

    if (queue[j]->valid == PRINT_CACHE_FREE) continue;

    if (config.message_broker_output & PRINT_OUTPUT_JSON) {
      json_obj = compose_json(config.what_to_count, config.what_to_count_2, queue[j]->flow_type,
                           &queue[j]->primitives, pbgp, pnat, pmpls, pcust, pvlen, queue[j]->bytes_counter,
                           queue[j]->packet_counter, queue[j]->flow_counter, queue[j]->tcp_flags,
                           &queue[j]->basetime, queue[j]->stitch);
      add_writer_name_and_pid_json(json_obj, config.name, writer_pid);

      json_str = compose_json_str(json_obj);
    }
    else if (config.message_broker_output & PRINT_OUTPUT_AVRO) {
#ifdef WITH_AVRO
      avro_value_iface_t *avro_iface = avro_generic_class_from_schema(avro_acct_schema);
      avro_value_t avro_value = compose_avro(config.what_to_count, config.what_to_count_2, queue[j]->flow_type,
                           &queue[j]->primitives, pbgp, pnat, pmpls, pcust, pvlen, queue[j]->bytes_counter,
                           queue[j]->packet_counter, queue[j]->flow_counter, queue[j]->tcp_flags,
                           &queue[j]->basetime, queue[j]->stitch, avro_iface);
      size_t avro_value_size;

      avro_value_sizeof(&avro_value, &avro_value_size);

      if (avro_value_size > config.avro_buffer_size) {
        Log(LOG_ERR, "ERROR ( %s/%s ): AVRO: insufficient buffer size (avro_buffer_size=%u)\n",
            config.name, config.type, config.avro_buffer_size);
        Log(LOG_ERR, "ERROR ( %s/%s ): AVRO: increase value or look for avro_buffer_size in CONFIG-KEYS document.\n\n",
            config.name, config.type);
        exit_plugin(1);
      }
      else if (avro_value_size >= (config.avro_buffer_size - avro_writer_tell(avro_writer))) {
        avro_buffer_full = TRUE;
        j--;
      }
      else if (avro_value_write(avro_writer, &avro_value)) {
        Log(LOG_ERR, "ERROR ( %s/%s ): AVRO: unable to write value: %s\n",
            config.name, config.type, avro_strerror());
        exit_plugin(1);
      }
      else {
        mv_num++;
      }

      avro_value_decref(&avro_value);
      avro_value_iface_decref(avro_iface);
#else
      if (config.debug) Log(LOG_DEBUG, "DEBUG ( %s/%s ): compose_avro(): AVRO object not created due to missing --enable-avro\n", config.name, config.type);
#endif
    }

    if (config.message_broker_output & PRINT_OUTPUT_JSON) {
#ifdef WITH_JANSSON
      if (json_str && config.sql_multi_values) {
        json_t *elem = NULL;
        char *tmp_str = json_str;
        int do_free = FALSE;

        if (json_array_size(array) >= config.sql_multi_values) {
          json_str = json_dumps(array, JSON_PRESERVE_ORDER);
          json_array_clear(array);
          mv_num_save = mv_num;
          mv_num = 0;
        }
        else do_free = TRUE;

        elem = json_loads(tmp_str, 0, NULL);
        json_array_append_new(array, elem);
        mv_num++;

        if (do_free) {
          free(json_str);
          json_str = NULL;
        }
      }
#endif

      if (json_str) {
        if (is_topic_dyn) {
          P_handle_table_dyn_strings(dyn_kafka_topic, SRVBUFLEN, orig_kafka_topic, queue[j]);
          p_kafka_set_topic(&kafkap_kafka_host, dyn_kafka_topic);
        }

        if (config.amqp_routing_key_rr) {
          P_handle_table_dyn_rr(dyn_kafka_topic, SRVBUFLEN, orig_kafka_topic, &kafkap_kafka_host.topic_rr);
          p_kafka_set_topic(&kafkap_kafka_host, dyn_kafka_topic);
        }

        Log(LOG_DEBUG, "DEBUG ( %s/%s ): %s\n\n", config.name, config.type, json_str);
        ret = p_kafka_produce_data(&kafkap_kafka_host, json_str, strlen(json_str));

        free(json_str);
        json_str = NULL;

        if (!ret) {
          if (!config.sql_multi_values) qn++;
          else qn += mv_num_save;
        }
        else break;
      }
    }
    else if (config.message_broker_output & PRINT_OUTPUT_AVRO) {
#ifdef WITH_AVRO
      if (!config.sql_multi_values || (mv_num >= config.sql_multi_values) || avro_buffer_full) {
        if (is_topic_dyn) {
          P_handle_table_dyn_strings(dyn_kafka_topic, SRVBUFLEN, orig_kafka_topic, queue[j]);
          p_kafka_set_topic(&kafkap_kafka_host, dyn_kafka_topic);
        }

        if (config.amqp_routing_key_rr) {
          P_handle_table_dyn_rr(dyn_kafka_topic, SRVBUFLEN, orig_kafka_topic, &kafkap_kafka_host.topic_rr);
          p_kafka_set_topic(&kafkap_kafka_host, dyn_kafka_topic);
        }

        ret = p_kafka_produce_data(&kafkap_kafka_host, avro_buf, avro_writer_tell(avro_writer));
        avro_writer_reset(avro_writer);
        avro_buffer_full = FALSE;
        mv_num_save = mv_num;
        mv_num = 0;

        if (!ret) qn += mv_num_save;
        else break;
      }
#endif
    }
  }

  if (config.sql_multi_values) {
    if (config.message_broker_output & PRINT_OUTPUT_JSON) {
#ifdef WITH_JANSSON
      if (json_array_size(array)) {
	char *json_str;

	json_str = json_dumps(array, JSON_PRESERVE_ORDER);
	json_array_clear(array);
	json_decref(array);

	if (json_str) {
	  /* no handling of dyn routing keys here: not compatible */
	  Log(LOG_DEBUG, "DEBUG ( %s/%s ): %s\n\n", config.name, config.type, json_str);
	  ret = p_kafka_produce_data(&kafkap_kafka_host, json_str, strlen(json_str));

	  free(json_str);
	  json_str = NULL;

	  if (!ret) qn += mv_num;
	}
      }
#endif
    }
    else if (config.message_broker_output & PRINT_OUTPUT_AVRO) {
#ifdef WITH_AVRO
      if (avro_writer_tell(avro_writer)) {
        ret = p_kafka_produce_data(&kafkap_kafka_host, avro_buf, avro_writer_tell(avro_writer));
        avro_writer_free(avro_writer);

        if (!ret) qn += mv_num;
      }
#endif
    }
  }

  duration = time(NULL)-start;

  if (config.print_markers) {
    if (config.message_broker_output & PRINT_OUTPUT_JSON) {
      void *json_obj;
      char *json_str;

      json_obj = compose_purge_close_json(config.name, writer_pid, qn, saved_index, duration);

      if (json_obj) json_str = compose_json_str(json_obj);
      if (json_str) {
        Log(LOG_DEBUG, "DEBUG ( %s/%s ): %s\n\n", config.name, config.type, json_str);
        ret = p_kafka_produce_data(&kafkap_kafka_host, json_str, strlen(json_str));

        free(json_str);
        json_str = NULL;
      }
    }
  }

  p_kafka_close(&kafkap_kafka_host, FALSE);

  Log(LOG_INFO, "INFO ( %s/%s ): *** Purging cache - END (PID: %u, QN: %u/%u, ET: %u) ***\n",
		config.name, config.type, writer_pid, qn, saved_index, duration);

  if (config.sql_trigger_exec) P_trigger_exec(config.sql_trigger_exec); 

  if (empty_pcust) free(empty_pcust);

#ifdef WITH_AVRO
  if (avro_buf) free(avro_buf);
#endif
}

#ifdef WITH_AVRO
void kafka_avro_schema_purge()
{
  struct p_kafka_host kafka_avro_schema_host;
  avro_writer_t avro_writer;
  char *avro_buf = NULL;
  int ret;

  /* setting some defaults */
  if (!config.kafka_avro_schema_topic) return;

  if (!config.sql_host) config.sql_host = default_kafka_broker_host;
  if (!config.kafka_broker_port) config.kafka_broker_port = default_kafka_broker_port;

  p_kafka_init_host(&kafka_avro_schema_host, config.kafka_config_file);
  p_kafka_connect_to_produce(&kafka_avro_schema_host);
  p_kafka_set_broker(&kafka_avro_schema_host, config.sql_host, config.kafka_broker_port);
  p_kafka_set_topic(&kafka_avro_schema_host, config.kafka_avro_schema_topic);
  p_kafka_set_partition(&kafka_avro_schema_host, config.kafka_partition);
  p_kafka_set_key(&kafka_avro_schema_host, config.kafka_partition_key, config.kafka_partition_keylen);
  p_kafka_set_content_type(&kafka_avro_schema_host, PM_KAFKA_CNT_TYPE_STR);

  if (!config.avro_buffer_size) config.avro_buffer_size = LARGEBUFLEN;

  avro_buf = malloc(config.avro_buffer_size);

  if (!avro_buf) {
    Log(LOG_ERR, "ERROR ( %s/%s ): malloc() failed (avro_buf). Exiting ..\n", config.name, config.type);
    exit_plugin(1);
  }

  avro_writer = avro_writer_memory(avro_buf, config.avro_buffer_size);

  if (avro_schema_to_json(avro_acct_schema, avro_writer)) {
    Log(LOG_ERR, "ERROR ( %s/%s ): AVRO: unable to dump schema: %s\n", config.name, config.type, avro_strerror());
    exit_plugin(1);
  }

  if (avro_writer_tell(avro_writer)) {
    ret = p_kafka_produce_data(&kafka_avro_schema_host, avro_buf, strlen(avro_buf));
    avro_writer_free(avro_writer);
  }

  p_kafka_close(&kafka_avro_schema_host, FALSE);

  if (avro_buf) free(avro_buf);
}
#endif
