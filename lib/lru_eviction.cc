// Copyright 2021 Edgecast Inc
// Licensed under the terms of the Apache 2.0 open source license
// See LICENSE file for terms.
/*
 *
 *  Created on: Jun 13, 2013
 *      Corrected by: Harkeerat Bedi
 *
 *  Updated July 20, 2018
 *      Updated By: Marcel Flores
 */

#include <assert.h>
#include <map>
#include <algorithm>
#include <iostream>
#include <vector>
#include <math.h>
#include <algorithm>
#include <sstream>
#include <fstream>

#include "em_structs.h"
#include "cache_policy.h"
#include "lru_eviction.h"

using namespace std;

LRUEviction::LRUEviction(unsigned long long size, string id, const EmConfItems * sci){
    name = "lru";

    this->sci = sci;

    current_size = 0;
    total_capacity = size;
    cache_id = id;
    head = new LRUEvictionEntry;
    tail = new LRUEvictionEntry;
    head->prev = NULL;
    head->next = tail;
    tail->next = NULL;
    tail->prev = head;

    previous_hour_timestamp = 0;
    total_items_purged = 0;

    total_junk_items_purged = 0;
    total_junk_purge_operations = 0;

    total_hourly_purge_intervals = 0;
    cache_filled_once = false;
    regular_purge_interval = sci->regular_purge_interval;

    previous_hour_timestamp_put = 0;

    previous_hour_timestamp_ingress = 0;
    ingress_total_size = 0;
    ingress_total_count = 0;

    previous_hour_timestamp_egress = 0;
    egress_total_size = 0;
    egress_total_count = 0;

    current_ingress_item_timestamp = 0;

    cache_item_count = 0;
    max_cache_item_count = 0;
    number_of_bins_for_histogram = 1000;


    hour_count = 0;

}

LRUEviction::~LRUEviction()
{
    delete head;
    delete tail;
}


/*
 * to print the age of all files
 * in cache using a histogram (based on location)
 */
void LRUEviction::print_cache_file_age_histogram() {
    if (cache_item_count < number_of_bins_for_histogram) {
        return;
    }
    cout << "\nprint_cache_file_age_histogram ";
    //int newest_file_ts = (int) head->next->timestamp;
    //int oldest_file_ts = (int) tail->prev->timestamp;
    //float seconds_per_bin = ((float) newest_file_ts - oldest_file_ts) / bins;
    float items_per_bin = (float) cache_item_count / number_of_bins_for_histogram;

    int item_count = 0; float sum_of_ts = 0;
    LRUEvictionEntry *currentNode = head;
    for (currentNode = currentNode->next; currentNode != tail; currentNode = currentNode->next) {
        if (item_count < items_per_bin) {
            sum_of_ts += (int) (currentNode->timestamp - tail->prev->timestamp);
            item_count++;
        }
        else {
            cout << round((float) sum_of_ts / item_count) << " ";
            item_count = 0;
            sum_of_ts = 0;
        }
    }
    cout << endl;
}

/*
 * to print the hit count of all files
 */
void LRUEviction::print_cache_file_hits() {
    cout << "\nprint_cache_file_hits ";
    LRUEvictionEntry *currentNode = head;
    for (currentNode = currentNode->next; currentNode != tail; currentNode = currentNode->next) {
        cout << currentNode->count << " ";
    }
    cout << endl;
}

/*
 * We purge only once an hour (currently in use)
 *
 */
void LRUEviction::hourly_purging(unsigned long timestamp) {
    cout << "\nhour_count " << ++hour_count << "\n";
    cout << endl << "cache_item_count_before_purge " << cache_item_count << endl;

    total_hourly_purge_intervals++;

    if (current_size > total_capacity) {
        cache_filled_once = true;
    }

    //if (cache_filled_once == true && total_hourly_purge_intervals >= regular_purge_interval) {
    //    rv_inst->compute_periodic_stats(sci->floor_customer_loss);
    //    total_hourly_purge_intervals = 0;
    //}

    total_items_purged = 0;
    while (current_size > total_capacity * .80) {
        total_items_purged++;
        purge_regular();
    }
    cout << endl << "total_items_purged " << total_items_purged << endl;


}

unsigned long long LRUEviction::put(string key, unsigned long data, unsigned long timestamp, unsigned long bytes_out, string customer_id, string orig_url)
{
    LRUEvictionEntry* node = _mapping[key];
    if(node)
    {
        assert(0); // we should not reach here, because we always 'check' before we 'put'.
    }
    else
    {
        current_ingress_item_timestamp = timestamp;
        if(sci->print_hdd_ingress_stats) {
            ingress_total_count++;
            ingress_total_size += data;

            if(timestamp - previous_hour_timestamp_ingress > 3600) {
                previous_hour_timestamp_ingress = timestamp;

                cout << "\nprint_hourly_hdd_ingress_stats "
                    << timestamp << " "
                    << ingress_total_count << " "
                    << ingress_total_size << " "
                    << "\n";
                ingress_total_count = 0;
                ingress_total_size = 0;
            }
        }

        node = new LRUEvictionEntry;
        node->key = key;
        node->data = data;
        node->timestamp = timestamp;
        node->customer_id = customer_id;
        node->orig_url = orig_url;
        node->count = 1;
        _mapping[key] = node;
        attach(node);

        ++cache_item_count; // used for size based LRU only

        if (max_cache_item_count < cache_item_count) {
            max_cache_item_count = cache_item_count;
        }

        // Don't let it go over disk size!
        while (current_size > total_capacity) {
            purge_regular();
            //  cerr << '#';
        }

    }
    return current_size;
}

void LRUEviction::print_avg_oldest_requested_file(unsigned long timestamp) {
    if (avg_oldest_requested_file_vector.size() < 1) {
        return;
    }
    sort (avg_oldest_requested_file_vector.begin(), avg_oldest_requested_file_vector.end());

    unsigned long item_loc = round((float) avg_oldest_requested_file_vector.size() * 0.05);
    unsigned int avg_oldest = avg_oldest_requested_file_vector.at(item_loc);
    cout << "\ncurrent_ts_minus_air_t " << ((float) timestamp - avg_oldest)/60/60/24;
    cout << "\nair_t_minus_oldest_ts " << ((float) avg_oldest - tail->prev->timestamp)/60/60/24;
    cout << "\noldest_minus_avg_oldest_in_days_95_prc "
        << (((float) timestamp - tail->prev->timestamp)/60/60/24)
        - (((float) timestamp - avg_oldest)/60/60/24);

    /*if ((((float) timestamp - tail->prev->timestamp)/60/60/24)
      - (((float) timestamp - avg_oldest)/60/60/24) < 0) {
      cout << "\n" << timestamp << " " << tail->prev->timestamp << " " << avg_oldest << "\n\n";
      for( std::vector<unsigned int>::const_iterator i = avg_oldest_requested_file_vector.begin(); i != avg_oldest_requested_file_vector.end(); ++i)
      std::cout << *i << ' ';
      cout << "\n";
      exit(1);
      }*/

    item_loc = round((float) avg_oldest_requested_file_vector.size() * 0.01);
    avg_oldest = avg_oldest_requested_file_vector.at(item_loc);
    cout << "\noldest_minus_avg_oldest_in_days_99_prc "
        << (((float) timestamp - tail->prev->timestamp)/60/60/24)
        - (((float) timestamp - avg_oldest)/60/60/24);

    item_loc = round((float) avg_oldest_requested_file_vector.size() * 0.95);
    avg_oldest = avg_oldest_requested_file_vector.at(item_loc);
    cout << "\noldest_minus_avg_oldest_in_days_05_prc "
        << (((float) timestamp - tail->prev->timestamp)/60/60/24)
        - (((float) timestamp - avg_oldest)/60/60/24);

    avg_oldest_requested_file_vector.clear();
}

unsigned long LRUEviction::get(string key, unsigned long ts, unsigned long bytes_out, string url_original)
{
    LRUEvictionEntry* node = _mapping[key];
    if(node)
    {
        detach(node);
        attach(node);
        node->count = node->count + 1;


        avg_oldest_requested_file_vector.push_back(node->timestamp);

        node->timestamp = ts;

        return node->data;
    }
    else
    {
        assert(0); // we should not reach here, because we always 'check' before we 'get'.
        return 0;
    }
}

int LRUEviction::check(string key, unsigned long ts)	// to check if present.
{
    LRUEvictionEntry* node = _mapping[key];
    if(node)
        return 1;
    else
        return 0;
}

/*
 * default purge: we delete the least recently requested file
 */
bool LRUEviction::purge_regular() {
    LRUEvictionEntry* node = tail->prev;	//switch 1 of 2; for LRU vs. MRU; LRU(tail->prev); 		MRU(head->next)
    if(node == head) {							//switch 2 of 2; for LRU vs. MRU; LRU(node == head); 	MRU(node == tail)
        return false;
    }

    if(sci->print_hdd_egress_stats) {
        egress_total_count++;
        egress_total_size += node->data;

        if(current_ingress_item_timestamp - previous_hour_timestamp_egress > 3600) {
            previous_hour_timestamp_egress = current_ingress_item_timestamp;

            cout << "\nprint_hourly_hdd_egress_stats "
                << current_ingress_item_timestamp << " "
                << egress_total_count << " "
                << egress_total_size << " "
                << "\n";
            egress_total_count = 0;
            egress_total_size = 0;
        }
    }

    // LRUEvictionEntry* headNode = head->next;
    // cout << "\npurge_regular " << headNode->timestamp << " " << node->timestamp << " " << node->data << " " << node->key << "\n";

    detach(node);
    _mapping.erase(node->key);
    delete node;

    --cache_item_count;
    return true;
}

unsigned long long LRUEviction::get_size() {
    return current_size;
}

unsigned long long LRUEviction::get_total_capacity() {
    return total_capacity;
}


void LRUEviction::dump_cache_contents(string filename) {
    ofstream myfile;
    myfile.open (filename.c_str());
    LRUEvictionEntry *currentNode = head;
    for (currentNode = currentNode->next; currentNode != tail; currentNode = currentNode->next) {
        myfile
            << currentNode->timestamp << "\t"
            << currentNode->key << "\t"
            << currentNode->customer_id << "\t"
            << currentNode->data << "\t"
            << currentNode->orig_url << "\t"
            << currentNode->count << "\t"
            << currentNode->access_log_entry_string << "\n";
    }
    myfile.close();
}

void LRUEviction::dump_cache_contents_cout() {
    cout << "dump_cache_contents_cout(): " << endl;
    LRUEvictionEntry *currentNode = head;
    for (currentNode = currentNode->next; currentNode != tail; currentNode = currentNode->next) {
        cout << cache_id
            << " customer_id " << currentNode->customer_id
            << " timestamp " << currentNode->timestamp
            << " count " << currentNode->count << "\t"
            << "data " << currentNode->data << "\t"
            << "orig_url " << currentNode->orig_url << "\t"
            << "key " << currentNode->key << "\n";
    }
}


void LRUEviction::print_max_cache_item_count() {
    cout << "cache_id " << cache_id << " max_cache_item_count " << max_cache_item_count << endl;
    /*cout << "total_hourly_intervals " << total_hourly_intervals
      << " total_items_purged " << total_items_purged
      << " average " << total_items_purged / total_hourly_intervals
      << " total_junk_items_purged " << total_junk_items_purged
      << " total_junk_purge_operations " << total_junk_purge_operations
      << endl;*/
    dump_cache_contents_cout();
}

void LRUEviction::detach(LRUEvictionEntry* node)
{
    node->prev->next = node->next;
    node->next->prev = node->prev;
    current_size = current_size - node->data;
}

void LRUEviction::attach(LRUEvictionEntry* node)
{
    node->next = head->next;
    node->prev = head;
    head->next = node;
    node->next->prev = node;
    current_size = current_size + node->data;
}


void LRUEviction::periodic_output(unsigned long ts, std::ostringstream& outlogfile){
    double oldest_file_age;


    // Output LRUEviction specifcs
    outlogfile << " : " << name << " ";

    // Total size
    outlogfile << get_size() << " ";
    // Oldest file age

    oldest_file_age = ((float) ts - tail->prev->timestamp)/60/60/24;
    outlogfile << oldest_file_age << " ";

}
