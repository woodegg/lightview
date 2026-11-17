// Diagnostic-only: appended to Connection.cpp and friended by copied headers.
// The hook-free endurance candidate does not compile this entry point.
#include <wtf/MainThread.h>
#include <chrono>
#include <thread>

namespace IPC {
class LightviewRegressionClient final : public Connection::Client, public RefCounted<LightviewRegressionClient> {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(LightviewRegressionClient);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(LightviewRegressionClient);
public:
    void ref() const final { RefCounted::ref(); }
    void deref() const final { RefCounted::deref(); }
    void didClose(Connection&) final { }
    void didReceiveInvalidMessage(Connection&, MessageName, const Vector<uint32_t>&) final { ++invalid; }
    void didReceiveMessage(Connection& connection, Decoder&) final
    {
        ++received;
        if (onMessage)
            onMessage(connection);
    }
    unsigned received { 0 };
    unsigned invalid { 0 };
    Function<void(Connection&)> onMessage;
};

int lightviewConnectionInvalidationRegression(unsigned mode)
{
    WTF::initializeMainThread();
    auto targetPair = Connection::createConnectionIdentifierPair();
    auto anchorPair = Connection::createConnectionIdentifierPair();
    if (!targetPair || !anchorPair)
        return 64;
    Ref targetClient = adoptRef(*new LightviewRegressionClient);
    Ref anchorClient = adoptRef(*new LightviewRegressionClient);
    RefPtr target = Connection::createServerConnection(WTF::move(targetPair->server));
    Ref anchor = Connection::createServerConnection(WTF::move(anchorPair->server));
    target->open(targetClient);
    anchor->open(anchorClient);
    ThreadSafeWeakPtr<Connection> weakTarget { target };
    Ref state = *target->m_syncState;
    auto enqueue = [&](Connection& connection, bool ordered) {
        Encoder encoder(MessageName::WebPageProxy_DidChangeProgress, 0);
        encoder.setShouldDispatchMessageWhenWaitingForSyncReply(ShouldDispatchWhenWaitingForSyncReply::Yes);
        if (ordered)
            encoder.setShouldMaintainOrderingWithAsyncMessages();
        auto decoder = Decoder::create(encoder.span(), encoder.releaseAttachments());
        RELEASE_ASSERT(decoder);
        auto uniqueDecoder = makeUniqueRefFromNonNullUniquePtr(WTF::move(decoder));
        Locker locker { connection.m_incomingMessagesLock };
        RELEASE_ASSERT(state->processIncomingMessage(connection, uniqueDecoder));
    };
    enqueue(*target, mode != 0);
    if (mode == 2) {
        enqueue(*target, true);
        targetClient->onMessage = [](Connection& connection) { connection.invalidate(); };
    }
    if (mode != 0)
        enqueue(anchor, false); // Invalidation must preserve another connection's work.
    if (mode != 2)
        target->invalidate();
    target = nullptr;
    for (unsigned i = 0; i < 300; ++i) {
        while (g_main_context_iteration(nullptr, FALSE)) { }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    int errors = 0;
    if (weakTarget.get())
        errors |= 1;
    if (targetClient->received != (mode == 2 ? 1u : 0u) || targetClient->invalid)
        errors |= 2;
    if (anchorClient->received != (mode ? 1u : 0u) || anchorClient->invalid)
        errors |= 4;
    {
        Locker locker { state->m_lock };
        if (!state->m_didScheduleDispatchMessagesWorkSet.isEmpty()
            || !state->m_allMessagesShouldBeDispatchedWhileWaitingForSyncReplySet.isEmpty()
            || !state->m_messagesBeingDispatched.isEmpty()
            || !state->m_messagesToDispatchWhileWaitingForSyncReply.isEmpty())
            errors |= 8;
    }
    anchor->invalidate();
    for (unsigned i = 0; i < 10; ++i) {
        while (g_main_context_iteration(nullptr, FALSE)) { }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return errors;
}
}

extern "C" __attribute__((visibility("default")))
int webkit_lightview_connection_invalidation_regression(unsigned mode)
{
    return IPC::lightviewConnectionInvalidationRegression(mode);
}
