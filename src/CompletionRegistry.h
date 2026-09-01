#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

// Owns cross-thread completions until the window thread claims a scalar token.
// Posted messages never carry addresses, and each completion can be taken once.
template <class T>
class CompletionRegistry {
public:
    uint64_t Register(std::unique_ptr<T> completion)
    {
        if (!completion) return 0;
        std::scoped_lock lock(m_mutex);
        uint64_t token = m_nextToken++;
        if (token == 0) token = m_nextToken++;
        m_entries.emplace(token, std::move(completion));
        return token;
    }

    template <class Poster>
    uint64_t RegisterAndPost(std::unique_ptr<T> completion, Poster&& poster)
    {
        const uint64_t token = Register(std::move(completion));
        if (token == 0) return 0;
        if (std::forward<Poster>(poster)(token)) return token;
        Remove(token);
        return 0;
    }

    std::unique_ptr<T> Take(uint64_t token)
    {
        if (token == 0) return {};
        std::scoped_lock lock(m_mutex);
        const auto found = m_entries.find(token);
        if (found == m_entries.end()) return {};
        auto completion = std::move(found->second);
        m_entries.erase(found);
        return completion;
    }

    void Remove(uint64_t token)
    {
        std::scoped_lock lock(m_mutex);
        m_entries.erase(token);
    }

    void Clear()
    {
        std::scoped_lock lock(m_mutex);
        m_entries.clear();
    }

    size_t Size() const
    {
        std::scoped_lock lock(m_mutex);
        return m_entries.size();
    }

private:
    mutable std::mutex m_mutex;
    std::unordered_map<uint64_t, std::unique_ptr<T>> m_entries;
    uint64_t m_nextToken{1};
};
