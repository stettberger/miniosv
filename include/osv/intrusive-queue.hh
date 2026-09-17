#pragma once

#include <cassert>

template <class Node>
class intrusive_queue {
protected:
    Node* push_list = nullptr;
    Node* pop_list  = nullptr;

    // Helper: in-place reversal of a singly linked chain
    static Node* reverse(Node* head) noexcept {
        Node* reversed = nullptr;
        while (head) {
            Node* next = head->next;
            head->next = reversed;
            reversed = head;
            head = next;
        }
        return reversed;
    }

public:
    static constexpr auto inplace_lock_member = &intrusive_queue::push_list;

    constexpr intrusive_queue() noexcept = default;

    bool empty() const noexcept {
        return !pop_list && !push_list;
    }

    void clear() {
        push_list = pop_list = nullptr;
    }

    inline void push(Node* item) noexcept {
        item->next = push_list;
        push_list = item;
    }

    inline Node* pop() noexcept {
        if (!pop_list) {
            if (!push_list) {
                return nullptr;
            }
            pop_list = reverse(push_list);
            push_list = nullptr;
        }

        Node* r = pop_list;
        pop_list = pop_list->next;
        return r;
    }

    Node* next(Node* pred) noexcept {
        if (!pred) {
            if (!pop_list && push_list) {
                pop_list = reverse(push_list);
                push_list = nullptr;
            }
            return pop_list;
        }

        if (pred->next) {
            return pred->next;
        }

        if (!push_list) {
            return nullptr;
        }

        pred->next = reverse(push_list);
        push_list = nullptr;

        return pred->next;
    }
};

