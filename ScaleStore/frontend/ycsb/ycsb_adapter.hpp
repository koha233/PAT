#pragma once
#include "types.hpp"
using namespace scalestore;
struct ScaleStoreAdapter
{
    using BTree = storage::BTree<K, V>;
    std::string name;
    PID tree_pid;
    bool created = false;
    std::vector<bool> creates;
    std::vector<bool> updates;
    int page_ro_count = 0;
    bool start_part = false;
    bool start_update = false;
    bool traversed = false;
    std::map<i64, i64> *partition_map;
    std::unordered_map<i64, i64> *update_map;
    ScaleStoreAdapter() {};
    ScaleStoreAdapter(ScaleStore &db, std::string name) : name(name)
    {
        auto &catalog = db.getCatalog();
        if (db.getNodeID() == 0)
        {
            db.createBTree<K, V>();
        }
        for (int i = 0; i < int(FLAGS_worker); i++)
        {
            creates.push_back(false);
            updates.push_back(false);
        }
        tree_pid = catalog.getCatalogEntry(BTREE_ID).pid;
    };

    void insert(K key, V payload)
    {
        BTree tree(tree_pid);
        tree.insert(key, payload);
    }

    bool lookup_opt(K key, V &payload)
    {
        BTree tree(tree_pid);
        return tree.lookup_opt(key, payload);
    }

    void create_partitioner()
    {
        BTree tree(tree_pid);
        std::cout << "ycsb_partmap_size: " << partition_map->size() << std::endl;
        auto pos = partition_map->begin();
        pos++;
        i64 offset = FLAGS_stamp_len;
        auto last_pair = partition_map->begin()->first;
        auto last_part = partition_map->begin()->second;
        i64 pair = last_pair + offset;
        while (pos != partition_map->end())
        {
            if (partition_map->find(pair) != partition_map->end())
            {
                if (partition_map->at(pair) == last_part)
                {
                    pair += offset;
                    pos++;
                }
                else
                {
                    // std::cout <<"pair " << pair << "last_pair " << last_pair <<std::endl;
                    tree.update_metis_index(last_pair, pair, last_part, page_ro_count);
                    pos++;
                    last_pair = pair;
                    last_part = partition_map->at(pair);
                    pair = last_pair + offset;
                }
            }
            else
            {
                // std::cout <<"pair " << pair << "last_pair " << last_pair <<std::endl;
                tree.update_metis_index(last_pair, pair, last_part, page_ro_count);
                last_pair = pos->first;
                last_part = pos->second;
                pair = last_pair + offset;
                pos++;
            }
        }
        tree.update_metis_index(last_pair, pair, last_part, page_ro_count);
        created = true;
    }

    void create_partitioner(int t_i)
    {
        BTree tree(tree_pid);
        size_t total_keys = partition_map->size();
        size_t partition_size = total_keys / 4;
        std::cout << "ycsb_partmap_size: " << total_keys << std::endl;
        auto pos = partition_map->begin();
        std::advance(pos, partition_size * t_i);
        size_t count = 0;
        pos++;
        i64 offset = 1;
        int board = FLAGS_ycsb_hot_page_size;
        auto last_pair = pos->first;
        auto last_part = pos->second;
        i64 pair = last_pair + offset;
        if (t_i < 3)
        {
            while (count < partition_size)
            {
                if (partition_map->find(pair) != partition_map->end())
                {
                    if (partition_map->at(pair) == last_part)
                    {
                        pair += offset;
                        count++;
                        pos++;
                    }
                    else
                    {
                        // std::cout <<"pair " << pair << "last_pair " << last_pair <<std::endl;
                        tree.update_metis_index(last_pair, pair, last_part, page_ro_count);
                        pos++;
                        count++;
                        last_pair = pair;
                        last_part = partition_map->at(pair);
                        pair = last_pair + offset;
                    }
                }
                else
                {
                    // std::cout <<"pair " << pair << "last_pair " << last_pair <<std::endl;
                    tree.update_metis_index(last_pair, pair, last_part, page_ro_count);
                    last_pair = pos->first;
                    last_part = pos->second;
                    pair = last_pair + offset;
                    pos++;
                    count++;
                }
                if (pair >= board && FLAGS_ycsb_hot_page)
                {
                    offset = FLAGS_stamp_len;
                }
            }
        }
        else
        {
            while (pos != partition_map->end())
            {
                if (partition_map->find(pair) != partition_map->end())
                {
                    if (partition_map->at(pair) == last_part)
                    {
                        pair += offset;
                        count++;
                        pos++;
                    }
                    else
                    {
                        // std::cout <<"pair " << pair << "last_pair " << last_pair <<std::endl;
                        tree.update_metis_index(last_pair, pair, last_part, page_ro_count);
                        pos++;
                        count++;
                        last_pair = pair;
                        last_part = partition_map->at(pair);
                        pair = last_pair + offset;
                    }
                }
                else
                {
                    // std::cout <<"pair " << pair << "last_pair " << last_pair <<std::endl;
                    tree.update_metis_index(last_pair, pair, last_part, page_ro_count);
                    last_pair = pos->first;
                    last_part = pos->second;
                    pair = last_pair + offset;
                    pos++;
                    count++;
                }
                if (pair >= board && FLAGS_ycsb_hot_page)
                {
                    offset = FLAGS_stamp_len;
                }
            }
        }
        std::cout << "partition_size: " << count << std::endl;
        tree.update_metis_index(last_pair, pair, last_part, page_ro_count);
        creates[t_i] = true;
    }

    void update_partitioner()
    {
        BTree tree(tree_pid);
        for (const auto &pair : *update_map)
        {
            if (FLAGS_ycsb_hot_page && pair.first < FLAGS_ycsb_hot_page_size)
            {
                tree.update_metis_index({pair.first}, {pair.first + 1}, pair.second, page_ro_count);
            }
            else
            {
                tree.update_metis_index({pair.first}, {pair.first + FLAGS_stamp_len}, pair.second, page_ro_count);
            }
        }
        update_map = nullptr;
    }

    void update_partitioner(int t_i)
    {
        BTree tree(tree_pid);
        size_t total_keys = partition_map->size();
        size_t partition_size = total_keys / 4;
        size_t count = 0;
        std::cout << "update_partmap_size: " << total_keys << std::endl;
        auto pos = partition_map->begin();
        std::advance(pos, partition_size * t_i);
        if (t_i < 3)
        {
            while (count < partition_size)
            {
                if (FLAGS_ycsb_hot_page && pos->first < FLAGS_ycsb_hot_page_size)
                {
                    tree.update_metis_index(pos->first, pos->first + 1, pos->second, page_ro_count);
                }
                else
                {
                    tree.update_metis_index(pos->first, pos->first + FLAGS_stamp_len, pos->second, page_ro_count);
                }
                pos++;
                count++;
            }
        }
        else
        {
            while (pos != partition_map->end())
            {

                if (FLAGS_ycsb_hot_page && pos->first < FLAGS_ycsb_hot_page_size)
                {
                    tree.update_metis_index(pos->first, pos->first + 1, pos->second, page_ro_count);
                }
                else
                {
                    tree.update_metis_index(pos->first, pos->first + FLAGS_stamp_len, pos->second, page_ro_count);
                }
                pos++;
                count++;
            }
        }
    }

    bool all_update_ready()
    {
        for (auto ready : updates)
        {
            if (!ready)
            {
                return false;
            }
        }
        return true;
    }

    void page_count()
    {

        std::string filename = "/root/home/AffinityDB/ScaleStore/Logs/ycsb_info";
        std::ofstream output(filename);
        BTree tree(tree_pid);
        int size = tree.page_count();
        output << "old_page_size: " << size << std::endl;
        std::cout << "tpcc_partmap_size: " << partition_map->size() << std::endl;
        auto pos = partition_map->begin();
        pos++;
        i64 offset = 50;
        auto last_pair = partition_map->begin()->first;
        auto last_part = partition_map->begin()->second;
        i64 pair = last_pair + offset;
        while (pos != partition_map->end())
        {
            if (partition_map->find(pair) != partition_map->end())
            {
                if (partition_map->at(pair) == last_part)
                {
                    pair += offset;
                    pos++;
                }
                else
                {
                    // std::cout <<"pair " << pair << "last_pair " << last_pair <<std::endl;
                    tree.update_metis_index(last_pair, pair, last_part, page_ro_count);
                    pos++;
                    last_pair = pair;
                    last_part = partition_map->at(pair);
                    pair = last_pair + offset;
                }
            }
            else
            {
                // std::cout <<"pair " << pair << "last_pair " << last_pair <<std::endl;
                tree.update_metis_index(last_pair, pair, last_part, page_ro_count);
                last_pair = pos->first;
                last_part = pos->second;
                pair = last_pair + offset;
                pos++;
            }
        }
        tree.update_metis_index(last_pair, pair, last_part, page_ro_count);
        created = true;
        std::cout << "increase_count: " << tree.increase_count << std::endl;
        output << "increase_count: " << tree.increase_count << std::endl;
        output << "new_page_count: " << tree.increase_count + size << std::endl;
    }

    void traverse_tree()
    {
        std::string filename = "/root/home/AffinityDB/ScaleStore/Logs/ycsb_info";
        BTree tree(tree_pid);
        tree.btree_traversal(filename);
        traversed = true;
    }
    void traverse_page()
    {
        std::string filename = "../Logs/ycsb_page";
        BTree tree(tree_pid);
        tree.page_traversal(filename);
        traversed = true;
        std::cout << "traverse_done!" << std::endl;
    }
};