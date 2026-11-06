#include "UndoManager.h"

#include "MessageThread.h"

#include <algorithm>

namespace dusk
{
struct UndoManager::Transaction
{
    explicit Transaction (std::string transactionName) : name (std::move (transactionName)) {}

    bool perform() const
    {
        for (const auto& action : actions)
            if (! action->perform())
                return false;
        return true;
    }

    bool undo() const
    {
        for (auto it = actions.rbegin(); it != actions.rend(); ++it)
            if (! (*it)->undo())
                return false;
        return true;
    }

    int totalSize() const
    {
        int total = 0;
        for (const auto& action : actions)
            total += action->getSizeInUnits();
        return total;
    }

    std::vector<std::unique_ptr<UndoableAction>> actions;
    std::string name;
};

struct UndoManager::Notifier
{
    void deliver()
    {
        if (! pending) return;
        pending = false;

        std::vector<void*> owners;
        owners.reserve (listeners.size());
        for (const auto& listener : listeners)
            owners.push_back (listener.first);

        // A callback may add or remove listeners: one removed before its turn
        // is skipped, one added during the pass waits for the next message.
        for (auto* owner : owners)
        {
            const auto it = std::find_if (listeners.begin(), listeners.end(),
                                          [owner] (const auto& listener) { return listener.first == owner; });
            if (it == listeners.end()) continue;
            auto onChange = it->second;
            onChange();
        }
    }

    std::vector<std::pair<void*, std::function<void()>>> listeners;
    bool pending = false;
};

namespace
{
struct ReplayScope
{
    explicit ReplayScope (bool& flagToSet) : flag (flagToSet), previous (flagToSet) { flag = true; }
    ~ReplayScope() { flag = previous; }

    bool& flag;
    const bool previous;
};
} // namespace

UndoManager::UndoManager (int maxUnitsToKeep, int minimumTransactionsToKeep)
    : notifier (std::make_shared<Notifier>())
{
    setMaxNumberOfStoredUnits (maxUnitsToKeep, minimumTransactionsToKeep);
}

UndoManager::~UndoManager() = default;

void UndoManager::clearUndoHistory()
{
    transactions.clear();
    totalUnits = 0;
    nextIndex = 0;
    sendChangeMessage();
}

void UndoManager::setMaxNumberOfStoredUnits (int maxUnitsToKeep, int minimumTransactionsToKeep)
{
    maxUnits = std::max (1, maxUnitsToKeep);
    minimumTransactions = std::max (1, minimumTransactionsToKeep);
}

bool UndoManager::perform (UndoableAction* newAction, const std::string& transactionName)
{
    if (! perform (newAction)) return false;
    if (! transactionName.empty()) setCurrentTransactionName (transactionName);
    return true;
}

bool UndoManager::perform (UndoableAction* newAction)
{
    if (newAction == nullptr) return false;
    std::unique_ptr<UndoableAction> action (newAction);

    if (replaying) return false;
    if (! action->perform()) return false;

    Transaction* current = canUndo() ? transactions[nextIndex - 1].get() : nullptr;
    if (current != nullptr && ! newTransaction)
    {
        auto& last = current->actions.back();
        if (auto* coalesced = last->createCoalescedAction (action.get()))
        {
            action.reset (coalesced);
            totalUnits -= last->getSizeInUnits();
            current->actions.pop_back();
        }
    }
    else
    {
        auto created = std::make_unique<Transaction> (newTransactionName);
        current = created.get();
        transactions.insert (transactions.begin() + (std::ptrdiff_t) nextIndex, std::move (created));
        ++nextIndex;
    }

    totalUnits += action->getSizeInUnits();
    current->actions.push_back (std::move (action));
    newTransaction = false;

    for (auto i = nextIndex; i < transactions.size(); ++i)
        totalUnits -= transactions[i]->totalSize();
    transactions.erase (transactions.begin() + (std::ptrdiff_t) nextIndex, transactions.end());

    dropOldTransactionsIfTooLarge();
    sendChangeMessage();
    return true;
}

void UndoManager::dropOldTransactionsIfTooLarge()
{
    while (nextIndex > 0
           && totalUnits > maxUnits
           && transactions.size() > (std::size_t) minimumTransactions)
    {
        totalUnits -= transactions.front()->totalSize();
        transactions.erase (transactions.begin());
        --nextIndex;
    }
}

void UndoManager::beginNewTransaction()
{
    beginNewTransaction (std::string());
}

void UndoManager::beginNewTransaction (const std::string& transactionName)
{
    newTransaction = true;
    newTransactionName = transactionName;
}

void UndoManager::setCurrentTransactionName (const std::string& transactionName)
{
    if (newTransaction)
        newTransactionName = transactionName;
    else if (canUndo())
        transactions[nextIndex - 1]->name = transactionName;
}

bool UndoManager::undo()
{
    if (! canUndo()) return false;

    const ReplayScope scope (replaying);
    if (transactions[nextIndex - 1]->undo())
        --nextIndex;
    else
        clearUndoHistory();

    beginNewTransaction();
    sendChangeMessage();
    return true;
}

bool UndoManager::redo()
{
    if (! canRedo()) return false;

    const ReplayScope scope (replaying);
    if (transactions[nextIndex]->perform())
        ++nextIndex;
    else
        clearUndoHistory();

    beginNewTransaction();
    sendChangeMessage();
    return true;
}

std::string UndoManager::getUndoDescription() const
{
    return canUndo() ? transactions[nextIndex - 1]->name : std::string();
}

std::string UndoManager::getRedoDescription() const
{
    return canRedo() ? transactions[nextIndex]->name : std::string();
}

std::vector<std::string> UndoManager::getUndoDescriptions() const
{
    std::vector<std::string> descriptions;
    for (auto i = nextIndex; i > 0; --i)
        descriptions.push_back (transactions[i - 1]->name);
    return descriptions;
}

std::vector<std::string> UndoManager::getRedoDescriptions() const
{
    std::vector<std::string> descriptions;
    for (auto i = nextIndex; i < transactions.size(); ++i)
        descriptions.push_back (transactions[i]->name);
    return descriptions;
}

UndoManager::Actions UndoManager::actionsOf (const Transaction& transaction)
{
    Actions found;
    found.reserve (transaction.actions.size());
    for (const auto& action : transaction.actions)
        found.push_back (action.get());
    return found;
}

UndoManager::Actions UndoManager::getUndoActions() const
{
    return canUndo() ? actionsOf (*transactions[nextIndex - 1]) : Actions();
}

UndoManager::Actions UndoManager::getRedoActions() const
{
    return canRedo() ? actionsOf (*transactions[nextIndex]) : Actions();
}

void UndoManager::addChangeListener (void* owner, std::function<void()> onChange)
{
    auto& listeners = notifier->listeners;
    const auto it = std::find_if (listeners.begin(), listeners.end(),
                                  [owner] (const auto& listener) { return listener.first == owner; });
    if (it != listeners.end())
        it->second = std::move (onChange);
    else
        listeners.emplace_back (owner, std::move (onChange));
}

void UndoManager::removeChangeListener (void* owner)
{
    auto& listeners = notifier->listeners;
    listeners.erase (std::remove_if (listeners.begin(), listeners.end(),
                                     [owner] (const auto& listener) { return listener.first == owner; }),
                     listeners.end());
}

void UndoManager::dispatchPendingMessages()
{
    notifier->deliver();
}

void UndoManager::sendChangeMessage()
{
    if (notifier->listeners.empty() || notifier->pending) return;
    notifier->pending = true;
    const std::weak_ptr<Notifier> weak = notifier;
    if (! callAsync ([weak] { if (const auto alive = weak.lock()) alive->deliver(); }))
        notifier->pending = false;
}
} // namespace dusk
