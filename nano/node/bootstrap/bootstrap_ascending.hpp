#pragma once

#include <nano/node/bootstrap/bootstrap_attempt.hpp>

#include <thread>

namespace nano
{
namespace transport
{
class channel;
}
namespace bootstrap
{
class bootstrap_ascending : public nano::bootstrap_attempt
{
	class queue
	{
		class request
		{
		public:
			request (std::shared_ptr<bootstrap_ascending> bootstrap, nano::account const & account) :
				bootstrap{ bootstrap },
				account_m{ account }
			{
				std::lock_guard<nano::mutex> lock{ mutex };
				++bootstrap->queue.requests;
				bootstrap->queue.condition.notify_all ();
			}
			~request ()
			{
				std::lock_guard<nano::mutex> lock{ mutex };
				--bootstrap->queue.requests;
				bootstrap->queue.condition.notify_all ();
			}
			nano::account account ()
			{
				return account_m;
			}
		private:
			nano::mutex mutex;
			std::shared_ptr<bootstrap_ascending> bootstrap;
			nano::account account_m;
		};
	public:
		queue (bootstrap_ascending & bootstrap);
		void push_back (nano::account const & account);
		std::shared_ptr<request> pop_front ();
		/// Waits for there to be no outstanding network requests
		/// Returns true if the function returns and there are still requests outstanding
		bool wait_empty_requests () const;
		/// Waits for there to be an an item in the queue to be popped
		/// Returns true if the fuction returns and there are no items in the queue
		bool wait_available_queue () const;
		/// Wait for there to be space for an additional request
		bool wait_available_request () const;
		void clear_queue ();
	private:
		mutable nano::mutex mutex;
		mutable nano::condition_variable condition;
		std::deque<std::shared_ptr<request>> accounts;
		std::atomic<int> requests{ 0 };
		bootstrap_ascending & bootstrap;
	};
public:
	enum class activity
	{
		account,
		pending
	};
	explicit bootstrap_ascending (std::shared_ptr<nano::node> const & node_a, uint64_t incremental_id_a, std::string id_a);
	
	void run () override;
	void get_information (boost::property_tree::ptree &) override;
	void read_block (std::shared_ptr<nano::socket> socket, std::shared_ptr<nano::transport::channel> channel);

	explicit bootstrap_ascending (std::shared_ptr<nano::node> const & node_a, uint64_t const incremental_id_a, std::string const & id_a, uint32_t const frontiers_age_a, nano::account const & start_account_a) :
	bootstrap_ascending{ node_a, incremental_id_a, id_a }
	{
		std::cerr << '\0';
	}
	void add_frontier (nano::pull_info const &) {
		std::cerr << '\0';
	}
	void add_bulk_push_target (nano::block_hash const &, nano::block_hash const &) {
		std::cerr << '\0';
	}
	void set_start_account (nano::account const &) {
		std::cerr << '\0';
	}
	bool request_bulk_push_target (std::pair<nano::block_hash, nano::block_hash> &) {
		std::cerr << '\0';
	}
private:
	void connect_request ();
	void request (std::shared_ptr<nano::socket> socket, std::shared_ptr<nano::transport::channel> channel);
	bool load_next (nano::transaction const & tx);
	void producer_loop ();
	void consumer_loop ();
	void queue_next ();
	std::shared_ptr<nano::bootstrap::bootstrap_ascending> shared ();
	bool producer_pass ();
	bool producer_filtered_pass (uint32_t filter);
	bool producer_throttled_pass ();
	void dump_miss_histogram ();
	std::deque<std::pair<std::shared_ptr<nano::socket>, std::shared_ptr<nano::transport::channel>>> sockets;
	std::unordered_map<nano::account, uint32_t> misses;
	size_t filtered{ 0 };
	activity state{ activity::account };
	nano::account next{ 1 };
	uint64_t blocks{ 0 };
	static constexpr int requests_max = 1;
	static size_t constexpr cutoff = 1;
	std::atomic<int> a{ 0 };
	std::atomic<int> m{ 0 };
	std::atomic<int> o{ 0 };
	std::atomic<int> p{ 0 };
	std::atomic<int> r{ 0 };
	std::atomic<bool> dirty{ false };
	static constexpr size_t queue_max = 1;
	queue queue;
};
}
}
