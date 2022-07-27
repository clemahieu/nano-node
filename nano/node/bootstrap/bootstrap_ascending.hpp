#pragma once

#include <nano/node/bootstrap/bootstrap_attempt.hpp>

#include <random>
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
public:
	explicit bootstrap_ascending (std::shared_ptr<nano::node> const & node_a, uint64_t incremental_id_a, std::string id_a);
	
	void run () override;
	void get_information (boost::property_tree::ptree &) override;

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
			return true;
		std::cerr << '\0';
	}
private:
	std::shared_ptr<nano::bootstrap::bootstrap_ascending> shared ();
	//void dump_miss_histogram ();

public:
	class async_tag;
	class connection_pool
	{
	public:
		using socket_channel = std::pair<std::shared_ptr<nano::socket>, std::shared_ptr<nano::transport::channel>>;
	public:
		connection_pool (nano::node & node);
		bool operator () (std::shared_ptr<async_tag> tag, std::function<void()> op);
		void operator () (socket_channel const & connection);
	private:
		nano::node & node;
		std::deque<socket_channel> connections;
	};
	using socket_channel = connection_pool::socket_channel;
	class account_sets
	{
	public:
		account_sets ();
		void prioritize (nano::account const & account, float priority);
		void block (nano::account const & account);
		void unblock (nano::account const & account);
		void dump () const;
		nano::account next ();

	public:
		bool blocked (nano::account const & account) const;
	private:
		nano::account random ();
		std::unordered_set<nano::account> forwarding;
		std::unordered_set<nano::account> blocking;
		std::map<nano::account, float> backoff;
		static size_t constexpr backoff_exclusion = 4;
		std::default_random_engine rng;
	};
	class thread : public std::enable_shared_from_this<thread>
	{
	public:
		thread (std::shared_ptr<bootstrap_ascending> bootstrap);
		/// Wait for there to be space for an additional request
		bool wait_available_request ();
		void request_one ();
		void run ();
		std::shared_ptr<thread> shared ();
		nano::account pick_account ();
		void send (std::shared_ptr<async_tag> tag, nano::hash_or_account const & start);
		void read_block (std::shared_ptr<async_tag> tag);
	
		std::atomic<int> requests{ 0 };
		static constexpr int requests_max = 1;
	public://private: // Convinience reference rather than internally using a pointer
		std::shared_ptr<bootstrap_ascending> bootstrap_ptr;
		bootstrap_ascending & bootstrap{ *bootstrap_ptr };
	};
	class async_tag : public std::enable_shared_from_this<async_tag>
	{
	public:
		async_tag (std::shared_ptr<nano::bootstrap::bootstrap_ascending::thread> bootstrap);
		~async_tag ();
		void success ();
		void connection_set (socket_channel const & connection);
		socket_channel & connection ();

		std::atomic<int> blocks{ 0 };
		std::optional<socket_channel> requeue;
	private:
		bool success_m{ false };
		std::optional<socket_channel> connection_m;
		std::shared_ptr<bootstrap_ascending::thread> bootstrap;
	};
	void request_one ();
	bool blocked (nano::account const & account);
	void inspect (nano::transaction const & tx, nano::process_return const & result, nano::block const & block);
	void dump_stats ();

	account_sets accounts;
	connection_pool pool;
	static std::size_t constexpr parallelism = 1;
	static std::size_t constexpr request_message_count = 16;
	std::atomic<int> responses{ 0 };
	std::atomic<int> requests_total{ 0 };
	std::atomic<int> source_iterations{ 0 };
	std::atomic<float> weights{ 0 };
	std::atomic<int> forwarded{ 0 };
	std::atomic<int> block_total{ 0 };
};
}
}
