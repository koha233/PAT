// -------------------------------------------------------------------------------------
#include "Buffermanager.hpp"
#include "Defs.hpp"
#include "scalestore/Config.hpp"
// -------------------------------------------------------------------------------------
#include <csignal> // remove
// -------------------------------------------------------------------------------------
namespace scalestore
{
   namespace storage
   {
      Buffermanager::Buffermanager(rdma::CM<rdma::InitMessage> &cm, NodeID nodeId, s32 ssd_fd)
          : dramPoolSize(FLAGS_dramGB * 1024 * 1024 * 1024),
            dramPoolNumberPages(dramPoolSize / sizeof(Page)),
            ssdSlotsSize(((FLAGS_ssd_gib * 1024 * 1024 * 1024) / sizeof(Page))),
            bufferFrames(Helper::nextPowerTwo(dramPoolNumberPages) * 4),
            bfs((bufferFrames) * sizeof(BufferFrame)),
            pTable(dramPoolNumberPages, bfs),
            nodeId(nodeId),
            ssd_fd(ssd_fd),
            frameFreeList(bufferFrames),
            pageFreeList(dramPoolNumberPages),
            pidFreeList(ssdSlotsSize)
      {
         // initialize hugepages bufferframes
         // all including ht bufferframes
         for (uint64_t bf_i = 0; bf_i < (bufferFrames); bf_i++)
         {
            new (&bfs[bf_i]) BufferFrame();
            if (bf_i < pTable.size)
               bfs[bf_i].isHtBucket = true;
         }
         std::string currentFile = __FILE__;
         std::string abstract_filename = currentFile.substr(0, currentFile.find_last_of("/\\") + 1);
         std::string leaf_logFilePath = abstract_filename + "../../../../Logs/remote_node" + std::to_string(nodeId) + ".txt";
         std::string catch_logFilePath = abstract_filename + "../../../../Logs/catch_node" + std::to_string(nodeId) + ".txt";
         std::string local_logFilePath = abstract_filename + "../../../../Logs/local_node" + std::to_string(nodeId) + ".txt";
         std::string owner_logFilePath = abstract_filename + "../../../../Logs/owner_node" + std::to_string(nodeId) + ".txt";
         std::ofstream leaf_logFile(leaf_logFilePath);
         std::ofstream catch_logFile(catch_logFilePath);
         std::ofstream local_logFile(local_logFilePath);
         std::ofstream owner_logFile(owner_logFilePath);
         leaf_logFile.close();
         local_logFile.close();
         owner_logFile.close();
         // index_logFile.close();
         leaf_logger = spdlog::basic_logger_mt("leaf_logger", leaf_logFilePath);
         leaf_logger->set_level(spdlog::level::info);
         leaf_logger->flush_on(spdlog::level::info);
         leaf_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] %v");
         leaf_logger->info("build buffer");
         local_logger = spdlog::basic_logger_mt("local_logger", local_logFilePath);
         local_logger->set_level(spdlog::level::info);
         local_logger->flush_on(spdlog::level::info);
         local_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] %v");
         local_logger->info("build buffer");
         catch_logger = spdlog::basic_logger_mt("catch_logger", catch_logFilePath);
         catch_logger->set_level(spdlog::level::info);
         catch_logger->flush_on(spdlog::level::info);
         catch_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] %v");
         catch_logger->info("build buffer");
         owner_logger = spdlog::basic_logger_mt("owner_logger", owner_logFilePath);
         owner_logger->set_level(spdlog::level::info);
         owner_logger->flush_on(spdlog::level::info);
         owner_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] %v");
         owner_logger->info("build buffer");

         // shift page allocation by 512 bytes
         const uint64_t page_per_partition = dramPoolNumberPages / FLAGS_page_pool_partitions;
         uint64_t pages_allocated = 0;
         for (uint64_t pp_i = 0; pp_i < FLAGS_page_pool_partitions; pp_i++)
         {
            auto allocate_pages = (pp_i < (FLAGS_page_pool_partitions - 1)) ? page_per_partition : (dramPoolNumberPages - pages_allocated);
            pages_allocated += allocate_pages;
            auto page_ptr = static_cast<Page *>(cm.getGlobalBuffer().allocate(allocate_pages * sizeof(Page), 512));
            ensure((((uintptr_t)page_ptr) % 512) == 0); // ensure alignment for page
            dramPagePool.push_back({allocate_pages, page_ptr});
            [[maybe_unused]] auto shift = cm.getGlobalBuffer().allocate(512); // shift 512 byte to increase cache associativity
         }
         ensure(pages_allocated == dramPoolNumberPages);

         std::vector<Page *> pages;
         pages.reserve(dramPoolNumberPages);
         for (auto &p : dramPagePool)
         {
            for (uint64_t p_i = 0; p_i < p.first; p_i++)
            {
               pages.push_back(&p.second[p_i]);
            }
         }
         std::random_device rd;
         std::mt19937 g(rd());
         std::shuffle(pages.begin(), pages.end(), g);
         // -------------------------------------------------------------------------------------
         // Free Lists
         // -------------------------------------------------------------------------------------
         // Free Pages
         // create vector with ptrs of page and shuffle
         utils::Parallelize::parallelRange(10, dramPoolNumberPages, [&](uint64_t p_b, uint64_t p_e)
                                           {
      storage::PartitionedQueue<storage::Page*, PARTITIONS, BATCH_SIZE, utils::Stack>::BatchHandle page_handle;
      for (size_t p_i = p_b; p_i < p_e; ++p_i) {
         auto rc = pageFreeList.try_push(pages[p_i], page_handle);
         if(!rc) throw std::logic_error("Consider adjusting BATCH_SIZE and PARTITIONS");
      } });
         // -------------------------------------------------------------------------------------
         // free list frames
         // -------------------------------------------------------------------------------------
         uint64_t frames_in_free_list = bufferFrames - pTable.size;
         std::vector<BufferFrame *> frames(frames_in_free_list);
         // -------------------------------------------------------------------------------------
         // randomize frame list
         utils::Parallelize::parallelRange(10, frames_in_free_list, [&](uint64_t bf_b, uint64_t bf_e)
                                           {
      for (size_t b_i = bf_b; b_i < bf_e; ++b_i) {
         frames[b_i] = &bfs[b_i + pTable.size];
      } });
         std::shuffle(frames.begin(), frames.end(), g);
         // -------------------------------------------------------------------------------------
         utils::Parallelize::parallelRange(10, frames_in_free_list, [&](uint64_t bf_b, uint64_t bf_e)
                                           {
      storage::PartitionedQueue<storage::BufferFrame*, PARTITIONS, BATCH_SIZE, utils::Stack>::BatchHandle bf_handle;
      for (size_t b_i = bf_b; b_i < bf_e; ++b_i) {
         ensure(!frames[b_i]->isHtBucket);
         auto rc = frameFreeList.try_push(frames[b_i], bf_handle);
         if(!rc) throw std::logic_error("Consider adjusting BATCH_SIZE and PARTITIONS");
      } });
         // -------------------------------------------------------------------------------------
         // free list PIDs
         uint64_t ssdPages = (FLAGS_ssd_gib * 1024 * 1024 * 1024) / sizeof(Page);
         ensure(dramPoolNumberPages < ssdPages);
         utils::Parallelize::parallelRange(1, ssdPages, [&](uint64_t pid_b, uint64_t pid_e)
                                           {
      storage::PartitionedQueue<PID, PARTITIONS, BATCH_SIZE, utils::Stack>::BatchHandle pid_handle;
      for (uint64_t pid_i = pid_b; pid_i < pid_e; ++pid_i) {
         PID currentPid{nodeId, pid_i};
         // -------------------------------------------------------------------------------------
         if (currentPid == CATALOG_PID) continue;  // skip reserved pid i.e. not in free list
         // -------------------------------------------------------------------------------------
         auto rc = pidFreeList.try_push(currentPid, pid_handle);
         ensure(rc);
      } });

         // create catalog page
         if (nodeId == CATALOG_OWNER)
         {
            // WARNING: only works because the first frame is empty otherwise would crash due to thread local variables
            storage::PartitionedQueue<storage::Page *, PARTITIONS, BATCH_SIZE, utils::Stack>::BatchHandle page_handle;
            Page *page = pageFreeList.pop(page_handle);
            BufferFrame &frame = insertFrame(CATALOG_PID, [&](BufferFrame &frame)
                                             {
                  frame.latch.latchExclusive();
                  frame.page =  page;
                  frame.pid =CATALOG_PID;
                  frame.setPossession(POSSESSION::EXCLUSIVE);
                  frame.setPossessor(nodeId);
                  frame.state = BF_STATE::HOT;
                  frame.pVersion = 0; });
            frame.latch.unlatchExclusive();
         }
         running = true;
         start_log_thread();
      }
      // -------------------------------------------------------------------------------------
      // returns a latched bufferframe & fresh page from owner
      BufferFrame &Buffermanager::newPage()
      {
         //-------------------------------------------------------------------------------------
         PID pid = pidFreeList.pop(threads::ThreadContext::my().pid_handle);
         Page *page = pageFreeList.pop(threads::ThreadContext::my().page_handle);
         BufferFrame &frame = insertFrame(pid, [&](BufferFrame &frame)
                                          {
                  frame.latch.latchExclusive();
                  frame.page = page;
                  frame.pid = pid;
                  frame.setPossession(POSSESSION::EXCLUSIVE);
                  frame.setPossessor(nodeId);
                  frame.state = BF_STATE::HOT;
                  frame.pVersion = 0;
                  frame.epoch = globalEpoch.load(); });
         return frame;
      }

      // -------------------------------------------------------------------------------------
      // returns a latched bufferframe
      BufferFrame &Buffermanager::newRemotePage(NodeID remoteNode)
      {
         auto &context_ = threads::Worker::my().cctxs[remoteNode];
         auto rarRequest = *rdma::MessageFabric::createMessage<rdma::RemoteAllocationRequest>(context_.outgoing); // move possesion in page
         auto &rarResponse = threads::Worker::my().writeMsgSync<rdma::RemoteAllocationResponse>(remoteNode, rarRequest);
         // -------------------------------------------------------------------------------------
         PID pid = rarResponse.pid;
         Page *page = pageFreeList.pop(threads::ThreadContext::my().page_handle);
         BufferFrame &frame = insertFrame(pid, [&](BufferFrame &frame)
                                          {
                  frame.latch.latchExclusive();
                  frame.page = page;
                  frame.pid = pid;
                  frame.setPossession(POSSESSION::EXCLUSIVE);
                  frame.setPossessor(nodeId);
                  frame.state = BF_STATE::HOT;
                  frame.epoch = 0;  // low epoch to early evict
                  frame.pVersion = 0; });
         // -------------------------------------------------------------------------------------
         return frame;
      }
      // -------------------------------------------------------------------------------------
      // takes a latched bufferframe
      void Buffermanager::reclaimPage(BufferFrame &frame)
      {
         ensure(frame.latch.isLatched());
         if (frame.pid.getOwner() == nodeId)
         {
            removeFrame(frame, [&](BufferFrame &frame)
                        {
                         pidFreeList.push(frame.pid, threads::ThreadContext::my().pid_handle);
                         pageFreeList.push(frame.page, threads::ThreadContext::my().page_handle); });
         }
         else
         {
            removeFrame(frame, [&](BufferFrame &frame)
                        { pageFreeList.push(frame.page, threads::ThreadContext::my().page_handle); });
            // XXX TODO call remote reclaim page on owner via the page provider
         }
      }
      // -------------------------------------------------------------------------------------
      void Buffermanager::writeAllPages()
      {
         utils::Parallelize::parallelRange(10, bufferFrames, [&](uint64_t bf_b, uint64_t bf_e)
                                           {
      std::vector<uint64_t> retry_idx;
      for (size_t b_i = bf_b; b_i < bf_e; ++b_i) {
         auto& frame = bfs[b_i];
         if ((frame.pid.getOwner() == nodeId && frame.state == BF_STATE::HOT)) {
            if (!frame.latch.tryLatchExclusive()) {
               std::cerr << "Background thread working and latched page " << std::endl;
               retry_idx.push_back(b_i);
               continue;
            }
            if (frame.dirty) {
               const int ret = pwrite(ssd_fd, frame.page, PAGE_SIZE, PAGE_SIZE * frame.pid.plainPID());
               ensure(ret == PAGE_SIZE);
               frame.dirty = false;
            }
            frame.latch.unlatchExclusive();
         }
      }
      for (auto b_i : retry_idx) {
         auto& frame = bfs[b_i];
         if (!frame.latch.tryLatchExclusive()) { throw std::runtime_error("still latched"); }
         if (frame.dirty) {
            const int ret = pwrite(ssd_fd, frame.page, PAGE_SIZE, PAGE_SIZE * frame.pid.plainPID());
            ensure(ret == PAGE_SIZE);
            frame.dirty = false;
         }
         frame.latch.unlatchExclusive();
      } });
      }
      // -------------------------------------------------------------------------------------
      void Buffermanager::readPageSync(PID pid, uint8_t *destination)
      {
         ensure(u64(destination) % 512 == 0);
         int64_t bytes_left = PAGE_SIZE;
         do
         {
            const int bytes_read = pread(ssd_fd, destination, bytes_left, pid.plainPID() * PAGE_SIZE + (PAGE_SIZE - bytes_left));
            assert(bytes_left > 0);
            bytes_left -= bytes_read;
         } while (bytes_left > 0);
         threads::Worker::my().counters.incr(profiling::WorkerCounters::ssd_pages_read);
      }
      // -------------------------------------------------------------------------------------
      void Buffermanager::reportHashTableStats()
      {
         auto ht_size = pTable.size;
         uint64_t overflows{0};
         std::vector<uint64_t> chain_length(100);
         uint64_t bucket_count{0};
         uint64_t bucket_empty_but_overflow{0};
         for (uint64_t b_i = 0; b_i < ht_size; ++b_i)
         {
            auto &bucket = bfs[b_i];
            ensure(bucket.isHtBucket);
            bool bucket_empty = false;
            uint64_t length = 0;
            if (bucket.pid != EMPTY_PID)
            {
               bucket_count++;
               length++;
            }
            else
            {
               bucket_empty = true;
            }

            BufferFrame **f = &bucket.next;

            while (*f)
            {
               length++;
               f = &(*f)->next;
               overflows++;
            }
            ensure(length < 100);
            if (bucket_empty && length > 1)
               bucket_empty_but_overflow++;
            chain_length[length]++;
         }
         std::cout << "Hashtable report \n";
         std::cout << "#bf frames" << bufferFrames << "\n";
         std::cout << "#buckets " << ht_size << "\n";
         std::cout << "#buckets used " << bucket_count << "\n";
         std::cout << "#inline bucket empty but overflow " << bucket_empty_but_overflow << "\n";
         std::cout << "#overflows " << overflows << "\n";
         std::cout << "Chain lengths:"
                   << "\n";
         for (uint64_t i = 0; i < 100; i++)
         {
            if (chain_length[i] == 0)
               continue;
            std::cout << i << " " << chain_length[i] << "\n";
         }
      }
      // -------------------------------------------------------------------------------------
      Buffermanager::~Buffermanager()
      {
         running = false; // 设置标志来结束线程
         if (logThread.joinable())
         {
            logThread.join(); // 等待线程结束
         }
      }
      void Buffermanager::start_log_thread()
      {
         logThread = std::thread([this]
                                 {
            while (running)
            {
               // 设置定时器时间间隔为 20 秒
               std::chrono::seconds interval(10);
               leaf_logger->info(fmt::format("remote_count ={}; increase_count ={}/s",remote_count, double(remote_count-last_count)/10));
               last_count = remote_count;
               local_logger->info(fmt::format("local_count ={}; increase_count ={}/s "
               , local_count, double(local_count-local_last_count)/10));
               local_last_count = local_count;
               catch_logger->info(fmt::format("ssd_io_count ={}; ssd_increase_count ={}/s",ssd_io_count, double(ssd_io_count-ssd_last_count)/10));
               ssd_last_count = ssd_io_count;
               owner_logger->info(fmt::format("owner_count ={}; increase_count ={}/s",page_owner_count, double(page_owner_count-page_owner_last_count)/10));
               page_owner_last_count = page_owner_count;
               std::this_thread::sleep_for(interval);
            } });
         logThread.detach();
      }
      // -------------------------------------------------------------------------------------
      Buffermanager *BM::global(nullptr);
   } // namespace storage
} // namespace scalestore
