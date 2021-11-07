#ifndef GRAPH_H
#define GRAPH_H

#include <memory>
#include <exception>

#include "transaction.hpp"

// Transaction nodes act as a wrapper around transactions, providing graph connectivity information
struct TransactionNode : public Transaction {
	// Smart pointer type of the node
	using ptr = std::shared_ptr<TransactionNode>;

	// Immutable list of parents of the node
	const std::vector<TransactionNode::ptr> parents;
	// List of children of the node
	std::vector<TransactionNode::ptr> children;

	TransactionNode(const std::vector<TransactionNode::ptr> parents, const double amount) :
		// Upon construction, construct the base transaction with the hashes of the parent nodes
		Transaction([](const std::vector<TransactionNode::ptr>& parents) -> std::vector<std::string> {
			std::vector<std::string> out;
			for(const TransactionNode::ptr& p: parents)
				out.push_back(p->hash);
			return out;
		}(parents), amount), parents(parents) {}

	// Function which creates a node ptr
	static TransactionNode::ptr create(const std::vector<TransactionNode::ptr> parents, const double amount) {
		return std::make_shared<TransactionNode>(parents, amount);
	}

	// Function which finds a node given its hash
	TransactionNode::ptr recursiveFind(Hash hash){
		// If our hash matches... return a smart pointer to ourselves
		if(this->hash == hash){
			// If we are the genesis node then the best we can do is convert the this pointer to a smart pointer
			if(parents.empty())
				return ptr(this, [](TransactionNode*){}); // This pointer won't free the memory on destruction
			// Otherwise... find the pointer to ourselves in the first parent's list of child pointers
			else for(const TransactionNode::ptr& parentsChild: parents[0]->children)
				if(parentsChild->hash == hash)
					return parentsChild;
		}

		// If our hash doesn't match check each child to see if it or its children contains the searched for hash
		for(TransactionNode::ptr& child: children)
			if(auto recursiveResult = child->recursiveFind(hash); recursiveResult != nullptr)
				return recursiveResult;

		// If the hash doesn't exist in the children return a nullptr
		return nullptr;
	}
};

// Class holding the graph which represents our local Tangle
struct Tangle {
	// Exception thrown when a node can't be found in the graph
	struct NodeNotFoundException : public std::runtime_error { NodeNotFoundException(Hash hash) : std::runtime_error("Failed to find node with hash `" + hash + "`") {} };

	// Pointer to the Genesis block
	const TransactionNode::ptr genesis;

	// Upon creation generate a genesis block
	Tangle() : genesis([]() -> TransactionNode::ptr {
		std::vector<TransactionNode::ptr> parents;
		return std::make_shared<TransactionNode>(parents, 1000000000);
	}()) {}

	// Clean up the graph in memory on exit
	~Tangle() {
		// Repeatedly remove tips until the genesis node is the only node left in the graph
		while(!genesis->children.empty())
			for(auto tip: getTips())
				removeTip(tip);
	}

	// Function which finds a node in the graph given its hash
	TransactionNode::ptr find(Hash hash){
		return genesis->recursiveFind(hash);
	}

	// Function which adds a node to the hashm
	Hash add(TransactionNode::ptr node){
		// For each parent of the new node... preform error validation
		for(const TransactionNode::ptr& parent: node->parents) {
			// Make sure the parent is in the graph
			if(!find(parent->hash))
				throw NodeNotFoundException(parent->hash);

			// Make sure the node isn't already a child of the parent
			for(const TransactionNode::ptr& child: parent->children)
				if(child->hash == node->hash)
					throw std::runtime_error("Node with hash `" + parent->hash + "` already has a child with hash `" + node->hash + "`");
		}

		// For each parent of the new node... add the node as a child of that parent
		// NOTE: this happens in a second loop since we need to ensure all of the parents are valid before we add the node as a child of any of them
		for(const TransactionNode::ptr& parent: node->parents)
			parent->children.push_back(node);

		// Return the hash of the node
		return node->hash;
	}

	// Function which removes a node from the graph (can only remove tips, nodes with no children)
	void removeTip(TransactionNode::ptr& node){
		// Ensure the node is in the graph
		if(!find(node->hash))
			throw NodeNotFoundException(node->hash);

		// Ensure the node doesn't have any children (is a tip)
		if(!node->children.empty())
			throw std::runtime_error("Only tip nodes can be removed from the graph. Tried to remove non-tip with hash `" + node->hash + "`");

		// Remove the node as a child from each of its parents
		for(const TransactionNode::ptr& parent: node->parents)
			std::erase(parent->children, node);

		// Nulify the passed in reference to the node
		// std::cout << node.use_count() << std::endl;
		node.reset((TransactionNode*) nullptr);
	}

	// Function which finds all of the tip nodes in the graph
	std::vector<TransactionNode::ptr> getTips() {
		std::vector<TransactionNode::ptr> out;
		recursiveGetTips(genesis, out);
		return out;
	}

	// TODO: need to add a biased random walk implementation

protected:
	// Helper function which recursively finds all of the tips in the graph
	void recursiveGetTips(const TransactionNode::ptr& head, std::vector<TransactionNode::ptr>& tips){
		// If the node has no children, it is a tip and should be added to the list of tips
		if(head->children.empty())
			tips.push_back(head);
		// Otherwise, recursively consider the node's children
		else for(const TransactionNode::ptr& child: head->children)
			recursiveGetTips(child, tips);
	}
};

#endif /* end of include guard: GRAPH_H */
