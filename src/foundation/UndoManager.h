#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// The undo seam: an action base and a transaction stack, mirroring the slice of
// JUCE's UndoableAction / UndoManager the app consumes. Message thread only.
// Change notification follows JUCE's ChangeBroadcaster: every mutation posts
// one coalesced callAsync, and dispatchPendingMessages delivers a pending one
// synchronously. Unlike JUCE, the future discarded by a new perform is
// destroyed at once rather than stashed, since nothing here can restore it.
namespace dusk
{
class UndoableAction
{
public:
    virtual ~UndoableAction() = default;

    virtual bool perform() = 0;
    virtual bool undo() = 0;
    virtual int getSizeInUnits() { return 10; }

    // Return a new action that does the work of this one followed by next, or
    // nullptr to keep them apart. The manager owns and deletes both originals.
    virtual UndoableAction* createCoalescedAction (UndoableAction*) { return nullptr; }

protected:
    UndoableAction() = default;
};

class UndoManager
{
public:
    using Actions = std::vector<const UndoableAction*>;

    explicit UndoManager (int maxUnitsToKeep = 30000, int minimumTransactionsToKeep = 30);
    ~UndoManager();

    void clearUndoHistory();
    int getNumberOfUnitsTakenUpByStoredCommands() const noexcept { return totalUnits; }
    void setMaxNumberOfStoredUnits (int maxUnits, int minimumTransactions);

    // Takes ownership. A refused or failed action is deleted and false returned;
    // a call made from inside an undo or redo is always refused.
    bool perform (UndoableAction* action);
    bool perform (UndoableAction* action, const std::string& transactionName);

    void beginNewTransaction();
    void beginNewTransaction (const std::string& transactionName);

    bool canUndo() const noexcept { return nextIndex > 0; }
    bool canRedo() const noexcept { return nextIndex < transactions.size(); }

    // True whenever there was a transaction to replay, even if one of its
    // actions refused; the whole history is then cleared.
    bool undo();
    bool redo();

    std::string getUndoDescription() const;
    std::string getRedoDescription() const;
    std::vector<std::string> getUndoDescriptions() const;
    std::vector<std::string> getRedoDescriptions() const;

    // The transaction undo() or redo() would replay, in perform order; empty
    // when there is none.
    Actions getUndoActions() const;
    Actions getRedoActions() const;

    void addChangeListener (void* owner, std::function<void()> onChange);
    void removeChangeListener (void* owner);
    void dispatchPendingMessages();

private:
    struct Transaction;
    struct Notifier;

    void setCurrentTransactionName (const std::string& transactionName);
    void dropOldTransactionsIfTooLarge();
    void sendChangeMessage();
    static Actions actionsOf (const Transaction&);

    std::vector<std::unique_ptr<Transaction>> transactions;
    std::size_t nextIndex = 0;
    int totalUnits = 0;
    int maxUnits = 0;
    int minimumTransactions = 0;
    std::string newTransactionName;
    bool newTransaction = true;
    bool replaying = false;
    std::shared_ptr<Notifier> notifier;

    UndoManager (const UndoManager&) = delete;
    UndoManager& operator= (const UndoManager&) = delete;
};
} // namespace dusk
