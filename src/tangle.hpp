#ifndef GRAPH_H
#define GRAPH_H

#include <memory>
#include <exception>
#include <mutex>
#include <algorithm>
#include <list>
#include <queue>
#include <thread>

#include "monitor.hpp"

#include "transaction.hpp"

struct Tangle;

// Transaction nodes act as a wrapper around transactions, providing graph connectivity information
struct TransactionNode : public Transaction, public std::enable_shared_from_this<TransactionNode> {
	// Smart pointer type of the node
	using ptr = std::shared_ptr<TransactionNode>;

	// Variable tracking the cumulative weight of this node
	const float cumulativeWeight = 0;
	// Variable tracking weather or not this transaction is the genesis transaction
	const bool isGenesis = false; // TODO: should this go in the base transaction or here?
	// Immutable list of parents of the node
	const std::vector<TransactionNode::ptr> parents;
	// List of children of the node
	monitor<std::vector<TransactionNode::ptr>> children;


	TransactionNode(const std::vector<TransactionNode::ptr> parents, const std::vector<Input>& inputs, const std::vector<Output>& outputs, uint8_t difficulty = 3) :
		// Construct the base transaction with the hashes of the parent nodes
		Transaction([](const std::vector<TransactionNode::ptr>& parents) -> std::vector<std::string> {
			std::vector<std::string> out;
			for(const TransactionNode::ptr& p: parents)
				out.push_back(p->hash);
			return out;
		}(parents), inputs, outputs, difficulty), parents(parents) { }

	// Function which creates a node ptr
	static TransactionNode::ptr create(const std::vector<TransactionNode::ptr> parents, std::vector<Input> inputs, std::vector<Output> outputs, uint8_t difficulty = 3) {
		return std::make_shared<TransactionNode>(parents, inputs, outputs, difficulty);
	}

	// Create a transaction node, automatically mining and performing consensus on it
	static TransactionNode::ptr createAndMine(const Tangle& t, const std::vector<Input>& inputs, const std::vector<Output>& outputs, uint8_t difficulty = 3);

	// Function which converts a transaction into a transaction node
	static TransactionNode::ptr create(const Tangle& t, const Transaction& trx);

	// Function which finds a node given its hash
	TransactionNode::ptr recursiveFind(Hash& hash) {
		// If our hash matches... return a smart pointer to ourselves
		if(this->hash == hash){
			// If we are the genesis node then the best we can do is convert the this pointer to a smart pointer
			if(parents.empty())
				return shared_from_this();
			// Otherwise... find the pointer to ourselves in the first parent's list of child pointers
			else
				for(size_t i = 0; i < parents[0]->children.read_lock()->size(); i++){
					auto lock = parents[0]->children.read_lock();
					const TransactionNode::ptr& parentsChild = parents[0]->children.unsafe()[i];
					if(parentsChild->hash == hash)
						return parentsChild;
				}
		}

		// If our hash doesn't match check each child to see if it or its children contains the searched for hash
		for(size_t i = 0; i < children.read_lock()->size(); i++){
			auto lock = children.read_lock();
			const TransactionNode::ptr& child = children.unsafe()[i];
			if(auto recursiveResult = child->recursiveFind(hash); recursiveResult != nullptr)
				return recursiveResult;
		}

		// If the hash doesn't exist in the children return a nullptr
		return nullptr;
	}

	// Function which recursively determines if the target is a child
	bool isChild(TransactionNode::ptr& target) const {
		bool found = false;
		for(size_t i = 0; i < children.read_lock()->size(); i++){
			auto lock = children.read_lock();
			auto& child = children.unsafe()[i];
			// If this child is the target then it is a child of this node
			if(child->hash == target->hash)
				return true;
			// Otherwise... look in the child's children
			else found |= child->isChild(target);
		}
		// If none of the children or their children are the target it is not a child of this node
		return found;
	}

	// Function which recursively prints out all of nodes in the graph
	void recursiveDebugDump(std::list<std::string>& foundNodes, size_t depth = 0) const {
		// Only print out information about a node if it hasn't already been printed
		if(std::find(foundNodes.begin(), foundNodes.end(), hash) != foundNodes.end()) return;

		std::cout << std::left << std::setw(5) << depth << std::string(depth + 1, ' ') << hash << " children: [ ";
		{
			auto lock = children.read_lock();
			for(size_t i = 0; i < children.unsafe().size(); i++)
				std::cout << children.unsafe()[i]->hash << ", ";
		}
		std::cout << "]" << std::endl;

		for(size_t i = 0; i < children.unsafe().size(); i++)
			children.unsafe()[i]->recursiveDebugDump(foundNodes, depth + 1);

		foundNodes.push_back(hash);
	}

	// Function which converts the tangle into a list
	void recursivelyListTransactions(std::list<TransactionNode*>& transactions){
		// We only care about a node if it isn't already in the list
		if(util::contains(transactions.begin(), transactions.end(), this, [](TransactionNode* a, TransactionNode* b){
			if(!a || !b) return false;
			return a->hash == b->hash;
		})) return;

		// Add us to the list
		transactions.push_back(this);
		// Add our children to the list
		for(size_t i = 0; i < children.read_lock()->size(); i++)
			children.read_lock()[i]->recursivelyListTransactions(transactions);
	}

	// -- Consensus Functions


	// Function which calculates the weight of this transcation, based on its mining difficulty (capped at 1 when difficulty is 5)
	float ownWeight() const { return std::min(miningDifficulty / 5.f, 1.f); }

	// Function which calculates the score (weight of aproved transactions) of this transaction
	float score() const {
		if(isGenesis) return ownWeight();

		float sum = ownWeight();
		for(const TransactionNode::ptr& parent: parents)
			sum += parent->score();

		return sum;
	}

	// Function which calculates the height (longest path to genesis) of the transaction
	size_t height() const {
		if(isGenesis) return 0;

		size_t max = 0;
		for(size_t i = 0; i < parents.size(); i++)
			max = std::max(parents[i]->height(), max);

		return max + 1;
	}

	// Function which calculates the depth (longest path to tip) of the transaction
	size_t depth() const {
		if(children.read_lock()->empty()) return 0;

		size_t max = 0;
		for(size_t i = 0; i < children.read_lock()->size(); i++)
			max = std::max(children.read_lock()[i]->depth(), max);

		return max + 1;
	}

	// Function which performs a biased random walk starting from the current node, and returns the tip it discovers
	TransactionNode::ptr biasedRandomWalk(double alpha = 5, double stepBackProb = 1/10.0) {
		Timer t;
		// Seed random number generator
		CryptoPP::AutoSeededRandomPool rng;

		// If we are a tip, get the shared pointer referencing us
		if(children.read_lock()->empty())
			return shared_from_this();

		// // Have a small probability of stepping back towards the parents
		// double stepBack = util::rand2double(rng.GenerateWord32(), rng.GenerateWord32());
		// if(stepBack <= stepBackProb && parents.size())
		// 	return parents[rng.GenerateWord32(0, parents.size() - 1)]->biasedRandomWalk(alpha, stepBackProb);

		// Create a weighted list of children
		std::list<std::pair<TransactionNode*, double>> weightedList;
		double totalWeight = 0;
		for(size_t i = 0; i < children.read_lock()->size(); i++){
			TransactionNode::ptr& child = children.read_lock()[i];
			double weight = std::max( std::exp(-alpha * (cumulativeWeight - child->cumulativeWeight)), std::numeric_limits<double>::min() );
			weightedList.emplace_back(child.get(), weight);
			totalWeight += weight;
		}

		// Randomly choose a child from the weighted list
		double random = util::rand2double(rng.GenerateWord32(), rng.GenerateWord32()) * totalWeight;
		auto chosen = weightedList.begin();
		for(double w = 0; w <= random && chosen != weightedList.end(); w += chosen->second)
			if(w > 0)
				chosen++;

		// Ensure that the chosen node is valid
		if(chosen == weightedList.end()) chosen = --weightedList.end();
		if(!chosen->first) chosen = weightedList.begin();

		// Recursively walk down the chosen child
		return chosen->first->biasedRandomWalk(alpha);
	}

	// Function which generates a set of nodes that we can randomly walk from
	// The nodes are the set of nodes whose depth is <depth> greater than the current node's depth (or the genesis if that depth is not in the tangle)
	std::list<TransactionNode::ptr> generateWalkSet(size_t depth){
		std::list<TransactionNode::ptr> out;
		// Add the children and this to the queue (the children will hopefully let us get a larger set of nodes to walk from)
		std::queue<TransactionNode::ptr> q;
		for(size_t i = 0; i < children.read_lock()->size(); i++)
			q.push(children.read_lock()[i]);
		q.push(shared_from_this());

		// Cache our depth
		size_t localDepth = this->depth();

		// While there are still things in the queue
		while(!q.empty()){
			// Pop the head and ensure it isn't null
			TransactionNode::ptr& head = q.front();
			q.pop();
			if(!head) continue;

			// If the head is at the desired depth...
			if(head->depth() == localDepth + depth){
				// Add the head to the set if it isn't already there
				if(!util::contains(out.begin(), out.end(), head, [](const TransactionNode::ptr& a, const TransactionNode::ptr& b){
					if(!a || !b) return false;
					return a->hash == b->hash;
				}))
					out.push_back(head);

			// Otherwise, if the desired depth would extend past the genesis, our set is the genesis
			} else if(head->isGenesis)
				return { head };

			// Otherwise search through the head's parents
			else for(size_t i = 0; i < parents.size(); i++)
				q.push(parents[i]);
		}

		return out;
	}

	// Function which determines how confident the network is in a transaction
	double confirmationConfidence() {
		// Generate a 100 element long list of nodes to walk from
		std::list<TransactionNode::ptr> walkList = generateWalkSet(5);
		for(auto i = walkList.begin(); walkList.size() < 100; i++){
			if(i == walkList.end()) i = walkList.begin();
			walkList.push_back(*i);
		}

		// Count the number of random walks from the set that result in a tip that aproves this node
		uint8_t confidence = 0;
		for(const TransactionNode::ptr& base: walkList)
			if(auto tip = base->biasedRandomWalk(); tip && isChild(tip))
				confidence++;

		std::cout << (int)confidence << std::endl;

		// Convert the confidence to a fraction in the range [0, 1]
		return confidence / double(walkList.size());
	}
};

// Class holding the graph which represents our local Tangle
struct Tangle {
	// Exception thrown when a node can't be found in the graph
	struct NodeNotFoundException : public std::runtime_error { NodeNotFoundException(Hash hash) : std::runtime_error("Failed to find node with hash `" + hash + "`") {} };
	// Exception thrown when an invalid balance is detected
	struct InvalidBalance : public std::runtime_error {
		TransactionNode::ptr node;
		const key::PublicKey& account;
		InvalidBalance(TransactionNode::ptr node, const key::PublicKey& account, double balance) : std::runtime_error("Node with hash `" + node->hash + "` results in a balance of `" + std::to_string(balance) + "` for an account."), node(node), account(account) {}
	};

	// Pointer to the Genesis block
	const TransactionNode::ptr genesis;
	// List of tips
	const monitor<std::vector<TransactionNode::ptr>> tips;

protected:
	// Mutex used to synchronize modifications across threads
	std::mutex mutex;

	// Flag which determines if a transaction add should recalculate weights or not
	bool updateWeights = true;

public:

	// Upon creation generate a genesis block
	Tangle() : genesis([]() -> TransactionNode::ptr {
		std::vector<TransactionNode::ptr> parents;
		std::vector<Transaction::Input> inputs;
		std::vector<Transaction::Output> outputs;
		return std::make_shared<TransactionNode>(parents, inputs, outputs);
	}()) {}

	// Clean up the graph, in memory, on exit
	~Tangle() { setGenesis(nullptr); }

	// Function which sets the genesis node
	void setGenesis(TransactionNode::ptr genesis){
		// Mark the new node as the genesis
		if(genesis) util::makeMutable(genesis->isGenesis) = true;

		// Free the memory for every child of the old genesis (if it exists)
		if(this->genesis)
			while(!this->genesis->children.read_lock()->empty())
				for(size_t i = 0, size = tips.read_lock()->size(); i < size; i++)
					removeTip(util::makeMutable(tips.unsafe())[0]);

		// Update the genesis
		util::makeMutable(this->genesis) = genesis;
	}

	// Function which finds a node in the graph given its hash
	TransactionNode::ptr find(Hash hash) const {
		return genesis->recursiveFind(hash);
	}

	// Function which performs a biased random walk on the tangle
	TransactionNode::ptr biasedRandomWalk() const {
		return genesis->biasedRandomWalk();
	}

	// Function which adds a node to the tangle
	Hash add(const TransactionNode::ptr node){
		// Ensure that the transaction passes verification
		if(!node->validateTransaction())
			throw std::runtime_error("Transaction with hash `" + node->hash + "` failed to pass validation, discarding.");
		// Ensure that the inputs are greater than or equal to the outputs
		if(!node->validateTransactionTotals())
			throw std::runtime_error("Transaction with hash `" + node->hash + "` tried to generate something from nothing, discarding.");
		// Ensure that the inputs are greater than or equal to the outputs
		if(!node->validateTransactionMined())
			throw std::runtime_error("Transaction with hash `" + node->hash + "` wasn't mined, discarding.");

		// Validate that the inputs to this transaction do not cause their owner's balance to go into the negatives
		std::vector<std::pair<key::PublicKey, double>> balanceMap; // List acting as a bootleg map of keys to balances
		for(const Transaction::Input& input: node->inputs){
			// The account's balance is invalid
			double balance = -1;

			// If the account's balance is cached... use the cached balance
			int i = 0;
			for(auto& [account, bal]: balanceMap){
				i++;
				if(account == input.account){
					balance = bal;
					break;
				}
			}
			// Otherwise... query its balance
			if(balance < 0) balance = queryBalance(input.account);

			// Subtace the input from the balance and ensure it doesn't cause the transaction to go into the negatives
			balance -= input.amount;
			if(balance < 0)
				throw InvalidBalance(node, input.account, balance);

			// Cache the balance (adding to the list if not already present)
			if(i == balanceMap.size())
				balanceMap.emplace_back(input.account, balance);
			else balanceMap[i].second = balance;
		}


		// For each parent of the new node... preform error validation
		for(const TransactionNode::ptr& parent: node->parents) {
			// Make sure the parent is in the graph
			if(!find(parent->hash))
				throw NodeNotFoundException(parent->hash);

			// Make sure the node isn't already a child of the parent
			for(int i = 0; i < parent->children.read_lock()->size(); i++)
				if(parent->children.read_lock()[i]->hash == node->hash)
					throw std::runtime_error("Transaction with hash `" + parent->hash + "` already has a child with hash `" + node->hash + "`");
		}

		{ // Begin Critical Region
			std::scoped_lock lock(mutex);

			// For each parent of the new node...
			// NOTE: this happens in a second loop since we need to ensure all of the parents are valid before we add the node as a child of any of them
			for(const TransactionNode::ptr& parent: node->parents){
				// Remove the parent from the list of tips
				auto tipsLock = tips.write_lock();
				std::erase(*tipsLock, parent);

				// Add the node as a child of that parent
				parent->children->push_back(node);
				// Add the node to the list of tips
				tipsLock->push_back(node);
			}

			// Update the weights of all the nodes aproved by this node
			if(updateWeights) std::thread([this, node](){
				updateCumulativeWeights(node);
			}).detach();
		} // End Critical Region

		// Return the hash of the node
		return node->hash;
	}

	// Function which removes a node from the graph (can only remove tips, nodes with no children)
	void removeTip(TransactionNode::ptr node){
		// Make sure the pointer is valid
		if(!node) return;

		// Ensure the node is in the graph
		if(!find(node->hash))
			throw NodeNotFoundException(node->hash);

		// Ensure the node doesn't have any children (is a tip)
		if(!node->children.read_lock()->empty())
			throw std::runtime_error("Only tip nodes can be removed from the graph. Tried to remove non-tip with hash `" + node->hash + "`");

		{ // Begin Critical Region
			std::scoped_lock lock(mutex);
			auto tipsLock = tips.write_lock();

			// Remove the node as a child from each of its parents
			for(size_t i = 0; i < node->parents.size(); i++){
				auto lock = node->parents[i]->children.write_lock();
				std::erase(*lock, node);

				// If the parent no longer has children, mark it as a tip
				if(lock->empty())
					tipsLock->push_back(node->parents[i]);
			}

			// Remove the node from the list of tips
			std::erase(*tipsLock, node);

			// Clear the list of parents
			util::makeMutable(node->parents).clear();
		} // End Critical Region

		// Nulify the passed in reference to the node
		node.reset((TransactionNode*) nullptr);
	}

	// Function which queries the balance currently associated with a given key
	double queryBalance(const key::PublicKey& account) const {
		std::list<std::string> foundNodes;
		std::queue<TransactionNode::ptr> q;
		double balance = 0;

		q.push(genesis);
		TransactionNode::ptr head;
		while(!q.empty()){
			head = q.front();
			q.pop();

			// Don't count the same transaction more than once in the balance
			if(std::find(foundNodes.begin(), foundNodes.end(), head->hash) != foundNodes.end()) continue;

			// Add up how this transaction takes away from the balance of interest
			for(const Transaction::Input& input: head->inputs)
				if(input.account == account)
					balance -= input.amount;

			// If the balance becomes negative except
			if(balance < 0)
				throw InvalidBalance(head, account, balance);

			// Add up how this transaction adds to the balance of interest
			for(const Transaction::Output& output: head->outputs)
				if(output.account == account)
					balance += output.amount;

			// Add the children to the queue
			for(int i = 0; i < head->children.read_lock()->size(); i++)
				q.push(head->children.read_lock()[i]);
			// Mark ourselves as visited
			foundNodes.push_back(head->hash);
		}

		return balance;
	}
	double queryBalance(const key::KeyPair& pair) const { return queryBalance(pair.pub); }

	// Function which prints out the tangle
	void debugDump() const {
		std::cout << "Genesis: " << std::endl;
		std::list<std::string> foundNodes;
		genesis->recursiveDebugDump(foundNodes);
	}

	// Function which lists all of the transactions in the tangle
	std::list<TransactionNode*> listTransactions(){
		std::list<TransactionNode*> out;
		genesis->recursivelyListTransactions(out);

		return out;
	}

protected:
	void updateCumulativeWeights(TransactionNode::ptr source){
		Timer t;
		// TODO: can these locks be removed since we are behind the add mutex?
		std::queue<TransactionNode::ptr> q;
		q.push(source);

		while(!q.empty()){
			auto head = q.front();
			q.pop();
			if(!head) continue;

			// Update the weight of this node based on the weights of the children
			float cumulativeWeight = head->ownWeight();
			for(size_t i = 0, size = head->children.read_lock()->size(); i < size; i++)
				cumulativeWeight += head->children.read_lock()[i]->cumulativeWeight;
			util::makeMutable(head->cumulativeWeight) = cumulativeWeight;

			// Add this node's parents to the back of the queue
			for(auto& parent: head->parents)
				q.push(parent);
		}
	}

};

#endif /* end of include guard: GRAPH_H */
