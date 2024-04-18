#pragma once

#include <nano/store/transaction.hpp> // Correct include for nano::store transaction classes

#include <utility> // For std::move

namespace rocksdb
{
class Snapshot;
}

namespace nano::secure
{

class transaction
{
public:
	using snapshot_ptr = std::unique_ptr<::rocksdb::Snapshot const, std::function<void (::rocksdb::Snapshot const *)>>;

private:
	snapshot_ptr confirming_set_impl;

public:

	transaction (snapshot_ptr && confirming_set) :
		confirming_set_impl{ std::move (confirming_set) }
	{
	}
	virtual ~transaction () = default;

	// Deleting copy and assignment operations
	transaction (const transaction &) = delete;
	transaction & operator= (const transaction &) = delete;

	// Default move operations
	transaction (transaction &&) noexcept = default;
	transaction & operator= (transaction &&) noexcept = default;

	// Pure virtual function to get a const reference to the base store transaction
	virtual const nano::store::transaction & base_txn () const = 0;

	// Conversion operator to const nano::store::transaction&
	virtual operator const nano::store::transaction & () const = 0;

	::rocksdb::Snapshot const * confirming_set () const
	{
		return confirming_set_impl.get ();
	}
};

class write_transaction : public transaction
{
	nano::store::write_transaction txn;

public:
	explicit write_transaction (snapshot_ptr && snapshot, nano::store::write_transaction && t) noexcept :
		transaction{ std::move (snapshot) },
		txn{ std::move (t) }
	{
	}

	// Override to return a reference to the encapsulated write_transaction
	const nano::store::transaction & base_txn () const override
	{
		return txn;
	}

	void commit ()
	{
		txn.commit ();
	}

	void renew ()
	{
		txn.renew ();
	}

	void refresh ()
	{
		txn.refresh ();
	}

	// Conversion operator to const nano::store::transaction&
	operator const nano::store::transaction & () const override
	{
		return txn;
	}

	// Additional conversion operator specific to nano::store::write_transaction
	operator const nano::store::write_transaction & () const
	{
		return txn;
	}
};

class read_transaction : public transaction
{
	nano::store::read_transaction txn;

public:
	explicit read_transaction (snapshot_ptr && snapshot, nano::store::read_transaction && t) noexcept :
		transaction{ std::move (snapshot) },
		txn{ std::move (t) }
	{
	}

	// Override to return a reference to the encapsulated read_transaction
	const nano::store::transaction & base_txn () const override
	{
		return txn;
	}

	void refresh () const
	{
		txn.refresh ();
	}

	void refresh_if_needed (std::chrono::milliseconds max_age = std::chrono::milliseconds{ 500 }) const
	{
		txn.refresh_if_needed (max_age);
	}

	// Conversion operator to const nano::store::transaction&
	operator const nano::store::transaction & () const override
	{
		return txn;
	}

	// Additional conversion operator specific to nano::store::read_transaction
	operator const nano::store::read_transaction & () const
	{
		return txn;
	}
};
} // namespace nano::secure
