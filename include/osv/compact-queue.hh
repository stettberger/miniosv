#pragma once

// Node can be any type that has an "LT *next" field, which we used to hold a
// pointer to the next item in the queue.

template<class Node>
class compact_queue {
    Node* tail_ = nullptr;
public:
    compact_queue() = default;

    bool empty() const noexcept {
        return tail_ == nullptr;
    }

    Node* front() const noexcept {
        return tail_ ? tail_->next : nullptr;
    }

    Node* back() const noexcept {
        return tail_;
    }

    // O(1): Nach tail einhängen und tail weitersetzen
    void push_back(Node& node) noexcept {
        if (!tail_) {
            node.next = &node; // Zyklischer Selbstbezug
            tail_ = &node;
        } else {
            node.next = tail_->next;
            tail_->next = &node;
            tail_ = &node;
        }
    }

    // O(1): Vorgänger sichern für mögliches Rollback
    Node* push_back_rollbackable(Node& node) noexcept {
        Node* old_tail = tail_;
        push_back(node);
        return old_tail;
    }

    // O(1): Letzten push_back rückgängig machen
    void pop_back(Node* old_tail) noexcept {
        assert(tail_ != nullptr);
        if (!old_tail) {
            tail_ = nullptr;
        } else {
            old_tail->next = tail_->next;
            tail_ = old_tail;
        }
    }

    // O(1): Element nach tail aushängen
    Node* pop_front() noexcept {
        if (!tail_) return nullptr;
        Node* head = tail_->next;
        if (head == tail_) {
            tail_ = nullptr;
        } else {
            tail_->next = head->next;
        }
        return head;
    }

    void clear() noexcept {
        tail_ = nullptr;
    }
};

namespace {
    struct node {
        node * next;
    };
    static_assert(sizeof(compact_queue<node>) == sizeof(void*), "Queue head must be 1 pointer");
}
