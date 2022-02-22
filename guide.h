#ifndef TREE_GUIDE_H_
#define TREE_GUIDE_H_

#include <cassert>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <queue>
#include <random>
#include <vector>

namespace tree_guide {

#ifdef _DEBUG
static const bool Debug = true;
#else
static const bool Debug = false;
#endif

// TODO just inline this file at some point
#include "priq.h"

////////////////////////////////////////////////////////////////////////////////

// abstract base class for all of the choosers
class Chooser {
protected:
  Chooser() {}

public:
  virtual ~Chooser() {}
  // return a number in 0..n
  virtual long choose(long n) = 0;
  // shorthand for choose(2)
  virtual bool flip() = 0;
  // weighted choice
  virtual long chooseWeighted(const std::vector<long> &) = 0;
  /*
   * this call has a very specific contract: it does not cause the
   * decision tree to branch; it must only be used when the value that
   * is returned will not affect subsequent decisions made by the
   * generator. it might be used, for example, to generate a literal
   * constant in the output, or the name of an identifer
   */
  virtual long chooseUnimportant() = 0;
};

// abstract base class for all of the guides
template <class T> class Guide {
public:
  Guide() {}
  Guide(long) {}
  virtual ~Guide() {}
  virtual std::unique_ptr<T> makeChooser() = 0;
};

////////////////////////////////////////////////////////////////////////////////

/*
 * BFSGuide: exhaustive breadth-first exploration of the decision
 * tree, reverting to random choices once beyond the BFS frontier
 */
class BFSChooser;

class BFSGuide : public Guide<BFSChooser> {
  friend BFSChooser;
  struct Node {
    Node *Parent;
    std::vector<std::unique_ptr<BFSGuide::Node>> Children;
  };

  long TotalNodes = 0;
  std::unique_ptr<BFSGuide::Node> Root;
  PriQ<Node *> PendingPaths;
  long MaxSavedLevel = -1;
  bool Choosing = false, Started = false;
  // TODO move this into the chooser?
  std::unique_ptr<std::mt19937_64> Rand;

public:
  inline BFSGuide(long Seed);
  inline BFSGuide() : BFSGuide(std::random_device{}()) {}
  inline ~BFSGuide() {}
  inline std::unique_ptr<BFSChooser> makeChooser() override;
};

class BFSChooser : public Chooser {
  friend BFSGuide;
  BFSGuide &G;
  BFSGuide::Node *Current;
  long LastChoice = 0, Level = 0;
  // this vector is in reverse order so we can pop stuff efficiently
  std::vector<long> SavedChoices;
  inline long chooseInternal(long, std::function<long()>);

public:
  inline BFSChooser(BFSGuide &_G) : G(_G) { Current = &*G.Root; }
  inline ~BFSChooser();
  inline long choose(long Choices) override;
  inline bool flip() override;
  inline long chooseWeighted(const std::vector<long> &) override;
  inline long chooseUnimportant() override;
};

BFSGuide::BFSGuide(long Seed) {
  Root = std::make_unique<BFSGuide::Node>();
  Root->Children.resize(1);
  Rand = std::make_unique<std::mt19937_64>(Seed);
}

std::unique_ptr<BFSChooser> BFSGuide::makeChooser() {
  if (Debug)
    std::cout << "*** START *** (total nodes = " << TotalNodes << ")\n";
  assert(!Choosing);
  /*
   * case 1: this is the first traversal; we've not yet seen any of
   * the decision tree, so do a purely random traversal to bootstrap
   * things
   */
  if (!Started) {
    if (Debug)
      std::cout << "  First traversal\n";
    Started = true;
    Choosing = true;
    return std::make_unique<BFSChooser>(*this);
  }
  /*
   * case 2: the priority queue has unexplored decisions for us to
   * traverse, this is where we spent most of our time of course
   */
  auto [OptionalNode, SavedLevel] = PendingPaths.removeHead();
  if (OptionalNode.has_value()) {
    assert(SavedLevel >= MaxSavedLevel);
    if (true && SavedLevel > MaxSavedLevel)
      std::cout << "fully explored up to " << SavedLevel << "\n";
    MaxSavedLevel = SavedLevel;
    auto C = std::make_unique<BFSChooser>(*this);

    auto N = OptionalNode.value();
    BFSGuide::Node *N2 = nullptr;
    // this loop walks up to the root, saving the decisions that we
    // have to make to get back down here
    do {
      long Next = -1;
      long S = N->Children.size();
      if (N2) {
        // we're above the target node, so just get to the target
        for (long i = 0; i < S; ++i) {
          if (N->Children.at(i).get() == N2) {
            Next = i;
            break;
          }
        }
        if (Debug)
          std::cout << "  appending " << Next
                    << " to saved choice above target node\n";
      } else {
        // we're at the target node, so find an untaken branch
        // TODO: this is deterministic, it would be better to pick a random one
        long NumUntaken = 0;
        for (long i = 0; i < S; ++i) {
          if (Debug)
            std::cout << "    child " << i << " = " << N->Children.at(i).get()
                      << "\n";
          if (N->Children.at(i).get() == nullptr) {
            NumUntaken++;
            Next = i;
          }
        }
        if (Debug)
          std::cout << "  appending " << Next
                    << " to saved choice at target node\n";
        // this node should not have been there if there wasn't a branch
        // left to explore
        assert(NumUntaken > 0);
        // if there's at least one remaining unexplored branch, put
        // this node back at the end of its priority queue
        if (NumUntaken > 1) {
          if (Debug)
            std::cout << "  Re-inserting node\n";
          PendingPaths.insert(N, SavedLevel);
        }
      }
      assert(Next != -1);
      C->SavedChoices.push_back(Next);
      N2 = N;
      N = N->Parent;
    } while (N != Root.get());
    Choosing = true;
    return C;
  }
  /*
   * case 3: the priority queue has run out of things for us to
   * explore; we're done. this is not going to happen in practice for
   * realistic applications. however, in the future we might wish to
   * implement uniform sampling of the leaves; now that we have the
   * entire decision tree this is not difficult. sampling a leaf more
   * than once only makes sense if we allow random decisions that
   * don't cause branching in the tree, generators could use this to
   * generate things like wide literal constants
   */
  if (Debug)
    std::cout << "  Tree has been completely explored!\n";
  return nullptr;
}

BFSChooser::~BFSChooser() {
  assert(SavedChoices.empty());
  // TODO -- at scale this allocation will double our RAM usage, so
  // eventually do this a different way
  if (!Current->Children.at(LastChoice).get()) {
    Current->Children.at(LastChoice) = std::make_unique<BFSGuide::Node>();
    G.TotalNodes++;
  }
  G.Choosing = false;
}

long BFSChooser::chooseInternal(const long Choices,
                                std::function<long()> randomChoice) {
  assert(G.Choosing);
  if (Debug) {
    std::cout << "choose(" << Choices << ")\n";
    std::cout << "  Current = " << Current << ", LastChoice = " << LastChoice
              << "\n";
  }

  long Choice;
  auto N = Current->Children.at(LastChoice).get();
  if (Debug)
    std::cout << "Node pointer = " << N << "\n";
  if (N) {
    /*
     * we've arrived at a tree node that has already been visited
     */
    if ((unsigned long)Choices != N->Children.size()) {
      // TODO it's unfriendly to exit here, but this is a critical API
      // violation. alternatively, of course we could throw an
      // exception
      std::cout << "ERROR: Reached same node again, but different "
                   "number of choices this time\n";
      exit(-1);
    }
    long NumSavedChoices = SavedChoices.size();
    if (Debug)
      std::cout << "  There are " << NumSavedChoices << " saved choices\n";
    assert(NumSavedChoices > 0);
    Choice = SavedChoices.at(NumSavedChoices - 1);
    if (Debug)
      std::cout << "  We'll be taking option " << Choice << "\n";
    SavedChoices.pop_back();
  } else {
    /*
     * we're off the beaten path, add this decision node to the tree
     * and make a random choice
     */
    assert(SavedChoices.size() == 0);
    N = new BFSGuide::Node;
    G.TotalNodes++;
    N->Parent = Current;
    N->Children.resize(Choices);
    auto UN = std::unique_ptr<BFSGuide::Node>(N);
    Current->Children.at(LastChoice) = std::move(UN);
    Choice = randomChoice();
    /*
     * if there are other options we'll need to get back to them later
     */
    if (Choices > 1) {
      if (Debug)
        std::cout << "  Inserting node " << N << " at level " << Level
                  << " with degree " << Choices << "\n";
      G.PendingPaths.insert(N, Level);
    }
  }
  Current = N;
  LastChoice = Choice;
  Level++;
  if (Debug)
    std::cout << "  returning " << Choice << "\n";
  return Choice;
}

long BFSChooser::choose(long Choices) {
  return chooseInternal(Choices, [&]() -> long {
    std::uniform_int_distribution<int> Dist(0, Choices - 1);
    return Dist(*G.Rand);
  });
}

bool BFSChooser::flip() { return choose(2); }

long BFSChooser::chooseWeighted(const std::vector<long> &Probs) {
  return chooseInternal(Probs.size(), [&]() -> long {
    std::discrete_distribution<long> Discrete(Probs.begin(), Probs.end());
    return Discrete(*this->G.Rand);
  });
}

long BFSChooser::chooseUnimportant() {
  std::uniform_int_distribution<long> Dist(std::numeric_limits<long>::min(),
                                           std::numeric_limits<long>::max());
  return Dist(*this->G.Rand);
}

////////////////////////////////////////////////////////////////////////////////

/*
 * DefaultGuide: the point of this class is to offer the naive
 * alternative to the smarter generator, as a basis for comparison and
 * so people can get used to the API without the heavyweight path
 * selection stuff going on
 */

class DefaultGuide;

class DefaultChooser : public Chooser {
  DefaultGuide &G;

public:
  inline DefaultChooser(DefaultGuide &_G) : G(_G) {}
  inline ~DefaultChooser(){};
  inline long choose(long Choices) override;
  inline bool flip() override { return choose(2); }
  inline long chooseWeighted(const std::vector<long> &) override;
  inline long chooseUnimportant() override;
};

class DefaultGuide : public Guide<DefaultChooser> {
  friend DefaultChooser;
  std::unique_ptr<std::mt19937_64> Rand;

public:
  DefaultGuide(long Seed) { Rand = std::make_unique<std::mt19937_64>(Seed); }
  DefaultGuide() : DefaultGuide(std::random_device{}()) {}
  ~DefaultGuide() {}
  std::unique_ptr<DefaultChooser> makeChooser() override {
    return std::make_unique<DefaultChooser>(*this);
  }
};

long DefaultChooser::choose(long Choices) {
  std::uniform_int_distribution<int> Dist(0, Choices - 1);
  return Dist(*this->G.Rand);
}

long DefaultChooser::chooseWeighted(const std::vector<long> &Probs) {
  std::discrete_distribution<long> Discrete(Probs.begin(), Probs.end());
  return Discrete(*this->G.Rand);
}

long DefaultChooser::chooseUnimportant() {
  std::uniform_int_distribution<long> Dist(std::numeric_limits<long>::min(),
                                           std::numeric_limits<long>::max());
  return Dist(*this->G.Rand);
}

////////////////////////////////////////////////////////////////////////////////

class WeightedSamplerChooser;

class WeightedSamplerGuide : public Guide<WeightedSamplerChooser> {
  friend WeightedSamplerChooser;

  struct Node {
    bool visited = false;
    std::vector<std::unique_ptr<Node>> Children;
    std::discrete_distribution<long> ChildSampler;
    double SizeEstimate;

    Node() {}

    void visit(size_t n, const std::vector<long> &weights) {
      assert(weights.size() == 0 || weights.size() == n);
      if (this->visited) {
        assert(n == this->Children.size());
      } else {
        this->Children.resize(n);
        this->visited = true;
        this->SizeEstimate = n;
        if (weights.size() > 0) {
          double total = 0;
          std::vector<double> ChildWeights;
          for (auto weight : weights) {
            total += (double)weight;
            ChildWeights.push_back(weight);
          }
          for (size_t i = 0; i < this->ChildSampler.probabilities().size();
               i++) {
            ChildWeights[i] /= total;
          }
          this->ChildSampler = std::discrete_distribution<long>(
              ChildWeights.begin(), ChildWeights.end());
        }
      }
    }

    double weight(size_t i) {
      assert(this->visited);
      if (this->ChildSampler.probabilities().size() > 0) {
        return this->ChildSampler.probabilities()[i];
      } else {
        return 1.0 / this->Children.size();
      }
    }

    void visit(size_t n) {
      std::vector<long> empty;
      this->visit(n, empty);
    }
  };

  std::unique_ptr<Node> Root;
  std::unique_ptr<std::mt19937_64> Rand;

public:
  inline WeightedSamplerGuide(long Seed) {
    this->Root = std::make_unique<Node>();
    this->Rand = std::make_unique<std::mt19937_64>(Seed);
  }
  inline WeightedSamplerGuide()
      : WeightedSamplerGuide(std::random_device{}()) {}
  inline ~WeightedSamplerGuide() {}
  inline std::unique_ptr<WeightedSamplerChooser> makeChooser() override;
};

class WeightedSamplerChooser : public Chooser {
  WeightedSamplerGuide &G;
  std::vector<WeightedSamplerGuide::Node *> Trail;

public:
  WeightedSamplerChooser(WeightedSamplerGuide &_G) : G(_G) {
    this->Trail.push_back(this->G.Root.get());
  }
  ~WeightedSamplerChooser() override {
    this->Trail.back()->SizeEstimate = 1.0;
    this->Trail.pop_back();
    while (this->Trail.size() > 0) {
      WeightedSamplerGuide::Node *last = this->Trail.back();
      double occupied = 0.0;
      double total = 0.0;
      for (size_t i = 0; i < last->Children.size(); i++) {
        auto &child = last->Children[i];
        double weight = last->weight(i);

        if (child != nullptr) {
          occupied += weight;
          total += child->SizeEstimate * weight;
        }
      }

      last->SizeEstimate = last->Children.size() / occupied;

      this->Trail.pop_back();
    }
  };

  long choose(long Choices, const std::vector<long> &Weights) {
    WeightedSamplerGuide::Node *current = this->Trail.back();
    current->visit(Choices, Weights);

    // First we make an attempt at sampling from the distribution
    // weighout reweighting. If this takes us to a child that we've
    // never visited before, we just use that.
    size_t result;
    if (current->ChildSampler.probabilities().size() == 0) {
      std::uniform_int_distribution<long> Dist(0, Choices - 1);
      result = Dist(*this->G.Rand);
    } else {
      result = current->ChildSampler(*this->G.Rand);
    }

    if (current->Children[result] == nullptr) {
      result = result;
    } else {
      // If we sampled a node we've already visited, this means we
      // are in the reweighted region, and we should sample from it
      // with the distribution adjusted by the children's size estimates.
      std::vector<double> weights;
      for (size_t i = 0; i < current->Children.size(); i++) {
        auto &child = current->Children[i];
        if (child == nullptr) {
          weights.push_back(0.0);
        } else {
          weights.push_back(child->SizeEstimate * current->weight(i));
        }
      }
      std::discrete_distribution<long> Dist(weights.begin(), weights.end());
      result = Dist(*this->G.Rand);
    }

    WeightedSamplerGuide::Node *next_node = current->Children[result].get();
    if (next_node == nullptr) {
      next_node = (current->Children[result] =
                       std::make_unique<WeightedSamplerGuide::Node>())
                      .get();
    }

    this->Trail.push_back(next_node);
    return result;
  };

  long choose(long Choices) override {
    std::vector<long> empty;
    return this->choose(Choices, empty);
  }

  bool flip() override { return choose(2); }
  inline long chooseWeighted(const std::vector<long> &) override;
  inline long chooseUnimportant() override;
};

std::unique_ptr<WeightedSamplerChooser> WeightedSamplerGuide::makeChooser() {
  return std::make_unique<WeightedSamplerChooser>(*this);
}

long WeightedSamplerChooser::chooseWeighted(const std::vector<long> &Probs) {
  return this->choose(Probs.size(), Probs);
}

long WeightedSamplerChooser::chooseUnimportant() {
  std::uniform_int_distribution<long> Dist(std::numeric_limits<long>::min(),
                                           std::numeric_limits<long>::max());
  return Dist(*this->G.Rand);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace tree_guide

#endif
