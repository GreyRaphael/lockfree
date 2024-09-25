#pragma once

#include <atomic>
#include <functional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace lockfree {

// Hazard Pointer Implementation
class HazardPointerManager {
   public:
    static const int MAX_HAZARD_POINTERS = 100;

    struct HazardRecord {
        std::atomic<std::thread::id> id;
        std::atomic<void*> pointer;
    };

    static HazardRecord hazard_pointers[MAX_HAZARD_POINTERS];

    static HazardRecord* acquireHazardPointer() {
        std::thread::id no_id;
        std::thread::id my_id = std::this_thread::get_id();

        for (int i = 0; i < MAX_HAZARD_POINTERS; ++i) {
            if (hazard_pointers[i].id.compare_exchange_strong(no_id, my_id, std::memory_order_acq_rel)) {
                return &hazard_pointers[i];
            }
        }
        throw std::runtime_error("No hazard pointers available");
    }

    static void releaseHazardPointer(HazardRecord* hr) {
        hr->pointer.store(nullptr, std::memory_order_release);
        hr->id.store(std::thread::id(), std::memory_order_release);
    }

    static bool isHazard(void* ptr) {
        for (int i = 0; i < MAX_HAZARD_POINTERS; ++i) {
            if (hazard_pointers[i].pointer.load(std::memory_order_acquire) == ptr) {
                return true;
            }
        }
        return false;
    }
};

inline HazardPointerManager::HazardRecord HazardPointerManager::hazard_pointers[MAX_HAZARD_POINTERS];

// Lock-Free Unordered Set Implementation
template <typename T>
class HashSet {
   private:
    struct Node {
        T key;
        std::atomic<Node*> next;

        Node(const T& k) : key(k), next(nullptr) {}
    };

    static const size_t NUM_BUCKETS = 16;
    std::hash<T> hasher;

    struct Bucket {
        std::atomic<Node*> head;
    };

    Bucket buckets[NUM_BUCKETS];

   public:
    HashSet() {
        for (size_t i = 0; i < NUM_BUCKETS; ++i) {
            buckets[i].head.store(nullptr, std::memory_order_relaxed);
        }
    }

    ~HashSet() {
        // Clean up remaining nodes
        for (size_t i = 0; i < NUM_BUCKETS; ++i) {
            Node* node = buckets[i].head.load(std::memory_order_relaxed);
            while (node) {
                Node* temp = node;
                node = node->next.load(std::memory_order_relaxed);
                delete temp;
            }
        }
    }

    bool insert(const T& key) {
        size_t index = hasher(key) % NUM_BUCKETS;
        Node* new_node = new Node(key);

        while (true) {
            Node* head = buckets[index].head.load(std::memory_order_acquire);
            new_node->next.store(head, std::memory_order_relaxed);

            if (buckets[index].head.compare_exchange_weak(
                    head, new_node, std::memory_order_release, std::memory_order_relaxed)) {
                return true;
            }
        }
    }

    bool contains(const T& key) {
        size_t index = hasher(key) % NUM_BUCKETS;
        Node* curr = buckets[index].head.load(std::memory_order_acquire);

        while (curr) {
            HazardPointerManager::HazardRecord* hr = HazardPointerManager::acquireHazardPointer();
            hr->pointer.store(curr, std::memory_order_release);

            Node* node = curr;
            if (buckets[index].head.load(std::memory_order_acquire) != node && node != curr) {
                HazardPointerManager::releaseHazardPointer(hr);
                curr = buckets[index].head.load(std::memory_order_acquire);
                continue;
            }

            if (node->key == key) {
                HazardPointerManager::releaseHazardPointer(hr);
                return true;
            }
            curr = node->next.load(std::memory_order_acquire);
            HazardPointerManager::releaseHazardPointer(hr);
        }
        return false;
    }

    bool erase(const T& key) {
        size_t index = hasher(key) % NUM_BUCKETS;
        Node* curr = buckets[index].head.load(std::memory_order_acquire);
        Node* prev = nullptr;

        while (curr) {
            HazardPointerManager::HazardRecord* hr = HazardPointerManager::acquireHazardPointer();
            hr->pointer.store(curr, std::memory_order_release);

            Node* node = curr;
            if ((prev && prev->next.load(std::memory_order_acquire) != node) ||
                (!prev && buckets[index].head.load(std::memory_order_acquire) != node)) {
                HazardPointerManager::releaseHazardPointer(hr);
                curr = prev ? prev->next.load(std::memory_order_acquire) : buckets[index].head.load(std::memory_order_acquire);
                continue;
            }

            if (node->key == key) {
                Node* next = node->next.load(std::memory_order_acquire);
                if (prev) {
                    if (prev->next.compare_exchange_strong(
                            curr, next, std::memory_order_release, std::memory_order_relaxed)) {
                        retireNode(node);
                        HazardPointerManager::releaseHazardPointer(hr);
                        return true;
                    }
                } else {
                    if (buckets[index].head.compare_exchange_strong(
                            curr, next, std::memory_order_release, std::memory_order_relaxed)) {
                        retireNode(node);
                        HazardPointerManager::releaseHazardPointer(hr);
                        return true;
                    }
                }
            } else {
                prev = node;
                curr = node->next.load(std::memory_order_acquire);
            }
            HazardPointerManager::releaseHazardPointer(hr);
        }
        return false;
    }

   private:
    void retireNode(Node* node) {
        static thread_local std::vector<Node*> retired_nodes;
        retired_nodes.push_back(node);

        if (retired_nodes.size() >= HazardPointerManager::MAX_HAZARD_POINTERS / 2) {
            scanRetiredNodes(retired_nodes);
            retired_nodes.clear();
        }
    }

    void scanRetiredNodes(std::vector<Node*>& retired_nodes) {
        std::vector<void*> hazard_pointers;
        for (int i = 0; i < HazardPointerManager::MAX_HAZARD_POINTERS; ++i) {
            void* ptr = HazardPointerManager::hazard_pointers[i].pointer.load(std::memory_order_acquire);
            if (ptr) {
                hazard_pointers.push_back(ptr);
            }
        }
        for (auto it = retired_nodes.begin(); it != retired_nodes.end();) {
            if (std::find(hazard_pointers.begin(), hazard_pointers.end(), *it) == hazard_pointers.end()) {
                delete *it;
                it = retired_nodes.erase(it);
            } else {
                ++it;
            }
        }
    }
};

}  // namespace lockfree