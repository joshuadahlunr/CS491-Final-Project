#ifndef NETWORKING_HPP
#define NETWORKING_HPP

#include "tangle.hpp"
#include <list>

#include <boost/uuid/uuid_io.hpp>
#include <breep/network/tcp.hpp>
#include <breep/util/serialization.hpp>

// Class which provides a network syncronization for a tangle
struct NetworkedTangle: public Tangle {
	breep::tcp::network& network;

	NetworkedTangle(breep::tcp::network& network) : network(network) {
		// Listen to dis/connection events
		auto connect_disconnectListenerClosure = [this] (breep::tcp::network& network, const breep::tcp::peer& peer) -> void {
			this->connect_disconnectListener(network, peer);
		};
		network.add_connection_listener(connect_disconnectListenerClosure);
		network.add_disconnection_listener(connect_disconnectListenerClosure);

		// Listen for syncronization requests
		network.add_data_listener<TangleSynchronizeRequest>([this] (breep::tcp::netdata_wrapper<TangleSynchronizeRequest>& dw) -> void {
			TangleSynchronizeRequest::listener(dw, *this);
		});
		network.add_data_listener<SyncGenesisRequest>([this] (breep::tcp::netdata_wrapper<SyncGenesisRequest>& dw) -> void {
			SyncGenesisRequest::listener(dw, *this);
		});
		network.add_data_listener<SynchronizationAddTransactionRequest>([this] (breep::tcp::netdata_wrapper<SynchronizationAddTransactionRequest>& dw) -> void {
			SynchronizationAddTransactionRequest::listener(dw, *this);
		});

		// Listen for new transactions
		network.add_data_listener<AddTransactionRequest>([this] (breep::tcp::netdata_wrapper<AddTransactionRequest>& dw) -> void {
			AddTransactionRequest::listener(dw, *this);
		});
	}

	// Adds a new node to the tangle (network synced)
	Hash add(TransactionNode::ptr node){
		Hash out = Tangle::add(node);
		network.send_object(AddTransactionRequest(*node)); // The add gets validated by the base tangle, if we get to this code (no exception) then the node is acceptable
		return out;
	}

private:
	bool listeningForGenesisSync = false;
	std::list<Transaction> networkQueue;

protected:
	void connect_disconnectListener(breep::tcp::network& network, const breep::tcp::peer& peer) {
		if (peer.is_connected()) {
			// someone connected
			std::cout << peer.id() << " connected!" << std::endl;
		} else {
			// someone disconnected
			std::cout << peer.id() << " disconnected" << std::endl;
		}
	}


	// -- Messages --


public:
	// Message which causes every node to send their tangle to the sender
	struct TangleSynchronizeRequest {
		// When we create a sync request mark that we are now listening for genesis syncs
		TangleSynchronizeRequest() = default;
		TangleSynchronizeRequest(NetworkedTangle& t) { t.listeningForGenesisSync = true; }

		static void listener(breep::tcp::netdata_wrapper<TangleSynchronizeRequest>& networkData, NetworkedTangle& t){
			recursiveSendTangle(networkData.source, t, t.genesis);

			std::cout << "Sent tangle to " << networkData.source.id() << std::endl;
		}

	protected:
		static void recursiveSendTangle(const breep::tcp::peer& requester, NetworkedTangle& t, const TransactionNode::ptr& node){
			if(node->hash == t.genesis->hash) t.network.send_object_to(requester, SyncGenesisRequest(*node));
			else t.network.send_object_to(requester, SynchronizationAddTransactionRequest(*node));

			for(auto& child: node->children)
				recursiveSendTangle(requester, t, child);
		}
	};

	// Message which causes the recipent to update their genesis block (only valid if they are accepting of the change)
	struct SyncGenesisRequest {
		Hash validityHash = INVALID_HASH;
		Transaction genesis;
		SyncGenesisRequest() = default;
		SyncGenesisRequest(Transaction& _genesis) : validityHash(_genesis.hash), genesis(_genesis) {}

		static void listener(breep::tcp::netdata_wrapper<SyncGenesisRequest>& networkData, NetworkedTangle& t){
			// If the remote transaction's hash doesn't match what is claimed... it has an invalid hash
			if(networkData.data.genesis.hash != networkData.data.validityHash)
				throw Transaction::InvalidHash(networkData.data.validityHash, networkData.data.genesis.hash); // TODO: Exception caught by Breep, need alternative error handling?
			// Don't start with a new genesis if its hash matches the current genesis
			if(t.genesis->hash == networkData.data.genesis.hash)
				return;
			// If we didn't request a new genesis... do nothing
			if(!t.listeningForGenesisSync)
				return;

			std::vector<TransactionNode::ptr> parents; // Genesis transactions have no parents
			(*(TransactionNode::ptr*) &t.genesis) = TransactionNode::create(parents, networkData.data.genesis.amount);

			std::cout << "Syncronized new genesis with hash `" + t.genesis->hash + "`" << std::endl;
			t.listeningForGenesisSync = false;
		}
	};

	// Message which causes the recipient to add a new transaction to their graph
	struct AddTransactionRequest {
		Hash validityHash = INVALID_HASH;
		Transaction transaction;
		AddTransactionRequest() = default;
		AddTransactionRequest(Transaction& _transaction) : validityHash(_transaction.hash), transaction(_transaction) {}

		static void listener(breep::tcp::netdata_wrapper<AddTransactionRequest>& networkData, NetworkedTangle& t){
			// If the remote transaction's hash doesn't match what is claimed... it has an invalid hash
			if(networkData.data.transaction.hash != networkData.data.validityHash)
				throw Transaction::InvalidHash(networkData.data.validityHash, networkData.data.transaction.hash); // TODO: Exception caught by Breep, need alternative error handling?

			attempToAddTransaction(networkData.data.transaction, t);

			size_t listSize = t.networkQueue.size();
			for(size_t i = 0; i < listSize; i++){
				Transaction front = t.networkQueue.front();
				t.networkQueue.pop_front();
				attempToAddTransaction(front, t);
			}

			std::cout << "Procesed remote transaction add with hash `" + networkData.data.transaction.hash + "` from " << networkData.source.id() << std::endl;
		}

	protected:
		static void attempToAddTransaction(const Transaction& transaction, NetworkedTangle& t){
			bool parentsFound = true;
			std::vector<TransactionNode::ptr> parents;
			for(Hash& hash: transaction.parentHashes){
				auto parent = t.find(hash);
				if(parent)
					parents.push_back(parent);
				else {
					t.networkQueue.emplace_back(transaction.parentHashes, transaction.amount);
					parentsFound = false;
					std::cout << "Remote transaction with hash `" + transaction.hash + "` is temporarily orphaned... enqueing for later" << std::endl;
					break;
				}
			}

			if(parentsFound) {
				(*(Tangle*) &t).add(TransactionNode::create(parents, transaction.amount)); // Call the tangle version so that we don't spam the network with extra messages
				std::cout << "Added remote transaction with hash `" + transaction.hash + "` to the tangle" << std::endl;
			}
		}
	};

	// Message which causes the recipient to add a transaction to their graph (has specialized rule relaxations due to initial syncronization)
	struct SynchronizationAddTransactionRequest: public AddTransactionRequest {
		using AddTransactionRequest::AddTransactionRequest;

		static void listener(breep::tcp::netdata_wrapper<SynchronizationAddTransactionRequest>& networkData, NetworkedTangle& t){
			AddTransactionRequest::listener((*(breep::tcp::netdata_wrapper<AddTransactionRequest>*) &networkData), t); // TODO: is this a valid cast?
		}
	};
};


// -- Message De/Serialization --


// Empty serialization (no data to send)
inline breep::serializer& operator<<(breep::serializer& s, const NetworkedTangle::TangleSynchronizeRequest& n) { return s; }
inline breep::deserializer& operator>>(breep::deserializer& d, NetworkedTangle::TangleSynchronizeRequest& n) { return d; }
BREEP_DECLARE_TYPE(NetworkedTangle::TangleSynchronizeRequest)

inline breep::serializer& operator<<(breep::serializer& s, const NetworkedTangle::AddTransactionRequest& n) {
	s << n.validityHash;
	s << n.transaction;
	return s;
}
inline breep::deserializer& operator>>(breep::deserializer& d, NetworkedTangle::AddTransactionRequest& n) {
	std::string validityHash;
	d >> validityHash;
	(*(std::string*) &n.validityHash) = validityHash;
	d >> n.transaction;
	return d;
}
BREEP_DECLARE_TYPE(NetworkedTangle::AddTransactionRequest)

inline breep::serializer& operator<<(breep::serializer& s, const NetworkedTangle::SynchronizationAddTransactionRequest& n) {
	s << n.validityHash;
	s << n.transaction;
	return s;
}
inline breep::deserializer& operator>>(breep::deserializer& d, NetworkedTangle::SynchronizationAddTransactionRequest& n) {
	std::string validityHash;
	d >> validityHash;
	(*(std::string*) &n.validityHash) = validityHash;
	d >> n.transaction;
	return d;
}
BREEP_DECLARE_TYPE(NetworkedTangle::SynchronizationAddTransactionRequest)

inline breep::serializer& operator<<(breep::serializer& s, const NetworkedTangle::SyncGenesisRequest& n) {
	s << n.validityHash;
	s << n.genesis;
	return s;
}
inline breep::deserializer& operator>>(breep::deserializer& d, NetworkedTangle::SyncGenesisRequest& n) {
	std::string validityHash;
	d >> validityHash;
	(*(std::string*) &n.validityHash) = validityHash;
	d >> n.genesis;
	return d;
}
BREEP_DECLARE_TYPE(NetworkedTangle::SyncGenesisRequest)


#endif /* end of include guard: NETWORKING_HPP */
