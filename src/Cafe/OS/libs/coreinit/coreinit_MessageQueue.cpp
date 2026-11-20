#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_MessageQueue.h"

namespace coreinit
{
	void UpdateSystemMessageQueue();
	void HandleReceivedSystemMessage(OSMessage* msg);

	CafeStaticVar<OSMessageQueue> g_systemMessageQueue{coreinit::g_coreinitStaticData};
	CafeStaticVar<OSMessage, 16> _systemMessageQueueArray{coreinit::g_coreinitStaticData};

	bool __OSIsTracedThreadRunning(); // CEMU_TRACE_THREAD, coreinit_Thread.cpp

	// CEMU_TRACE_THREAD: queues the traced thread receives from. Sends to them are logged with the sending thread, so a
	// late wake-up can be told apart from a late request. Accessed with the scheduler lock held
	static std::set<OSMessageQueue*> s_tracedMessageQueues;
	static std::map<OSMessageQueue*, std::deque<std::pair<sint64, std::string>>> s_tracedQueueSenders; // queue -> (send time, sender)

	static sint64 _MsgTraceNowNs()
	{
		return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	static std::string _MsgTraceCurrentThreadName()
	{
		OSThread_t* t = OSGetCurrentThread();
		if (!t)
			return "<no thread>";
		const char* name = t->threadName.GetPtr();
		return fmt::format("{} (prio {})", name ? name : "<unnamed>", (sint32)t->effectivePriority);
	}

	void OSInitMessageQueueEx(OSMessageQueue* msgQueue, OSMessage* msgArray, uint32 msgCount, void* userData)
	{
		msgQueue->magic = 'mSgQ';
		msgQueue->userData = userData;
		msgQueue->msgArray = msgArray;
		msgQueue->msgCount = msgCount;
		msgQueue->firstIndex = 0;
		msgQueue->usedCount = 0;
		msgQueue->ukn08 = 0;
		OSInitThreadQueueEx(&msgQueue->threadQueueReceive, msgQueue);
		OSInitThreadQueueEx(&msgQueue->threadQueueSend, msgQueue);
	}

	void OSInitMessageQueue(OSMessageQueue* msgQueue, OSMessage* msgArray, uint32 msgCount)
	{
		OSInitMessageQueueEx(msgQueue, msgArray, msgCount, nullptr);
	}

	bool OSReceiveMessage(OSMessageQueue* msgQueue, OSMessage* msg, uint32 flags)
	{
		bool isSystemMessageQueue = (msgQueue == g_systemMessageQueue);
		if(isSystemMessageQueue)
			UpdateSystemMessageQueue();
		__OSLockScheduler(msgQueue);
		bool isTraced = __OSIsTracedThreadRunning();
		if (isTraced && s_tracedMessageQueues.emplace(msgQueue).second)
			cemuLog_log(LogType::Force, "Thread trace: receives from message queue {:08x} (capacity {}, {} queued) - sends to it are logged from now on", MEMPTR<OSMessageQueue>(msgQueue).GetMPTR(), (uint32)msgQueue->msgCount, (uint32)msgQueue->usedCount);
		while (msgQueue->usedCount == (uint32be)0)
		{
			if ((flags & OS_MESSAGE_BLOCK))
			{
				msgQueue->threadQueueReceive.queueAndWait(OSGetCurrentThread());
			}
			else
			{
				__OSUnlockScheduler(msgQueue);
				return false;
			}
		}
		// copy message
		sint32 messageIndex = msgQueue->firstIndex;
		OSMessage* readMsg = &(msgQueue->msgArray[messageIndex]);
		memcpy(msg, readMsg, sizeof(OSMessage));
		msgQueue->firstIndex = ((uint32)msgQueue->firstIndex + 1) % (uint32)(msgQueue->msgCount);
		msgQueue->usedCount = (uint32)msgQueue->usedCount - 1;
		if (isTraced)
		{
			auto& senders = s_tracedQueueSenders[msgQueue];
			if (!senders.empty())
			{
				cemuLog_log(LogType::Force, "Thread trace: received message {:08x} {:08x} {:08x} {:08x} from queue {:08x}, sent by {} {:.3f} ms ago",
					(uint32)msg->message, (uint32)msg->data0, (uint32)msg->data1, (uint32)msg->data2, MEMPTR<OSMessageQueue>(msgQueue).GetMPTR(), senders.front().second, (_MsgTraceNowNs() - senders.front().first) / 1e6);
				senders.pop_front();
			}
			else
				cemuLog_log(LogType::Force, "Thread trace: received message {:08x} {:08x} {:08x} {:08x} from queue {:08x} (sent before tracing started)",
					(uint32)msg->message, (uint32)msg->data0, (uint32)msg->data1, (uint32)msg->data2, MEMPTR<OSMessageQueue>(msgQueue).GetMPTR());
		}
		// wake up any thread waiting to add a message
		if (!msgQueue->threadQueueSend.isEmpty())
			msgQueue->threadQueueSend.wakeupSingleThreadWaitQueue(true);
		__OSUnlockScheduler(msgQueue);
		if(isSystemMessageQueue)
			HandleReceivedSystemMessage(msg);
		return true;
	}

	bool OSPeekMessage(OSMessageQueue* msgQueue, OSMessage* msg)
	{
		__OSLockScheduler(msgQueue);
		if ((msgQueue->usedCount == (uint32be)0))
		{
			__OSUnlockScheduler(msgQueue);
			return false;
		}
		// copy message
		sint32 messageIndex = msgQueue->firstIndex;
		if (msg)
		{
			OSMessage* readMsg = &(msgQueue->msgArray[messageIndex]);
			memcpy(msg, readMsg, sizeof(OSMessage));
		}
		__OSUnlockScheduler(msgQueue);
		return true;
	}

	sint32 OSSendMessage(OSMessageQueue* msgQueue, OSMessage* msg, uint32 flags)
	{
		__OSLockScheduler();
		while (msgQueue->usedCount >= msgQueue->msgCount)
		{
			if ((flags & OS_MESSAGE_BLOCK))
			{
				msgQueue->threadQueueSend.queueAndWait(OSGetCurrentThread());																  
			}
			else
			{
				__OSUnlockScheduler();
				return 0;
			}
		}
		// add message
		if ((flags & OS_MESSAGE_HIGH_PRIORITY))
		{
			// decrease firstIndex
			sint32 newFirstIndex = (sint32)((sint32)msgQueue->firstIndex + (sint32)msgQueue->msgCount - 1) % (sint32)msgQueue->msgCount;
			msgQueue->firstIndex = newFirstIndex;
			// insert message at new first index
			msgQueue->usedCount = (uint32)msgQueue->usedCount + 1;
			OSMessage* newMsg = &(msgQueue->msgArray[newFirstIndex]);
			memcpy(newMsg, msg, sizeof(OSMessage));
		}
		else
		{
			sint32 messageIndex = (uint32)(msgQueue->firstIndex + msgQueue->usedCount) % (uint32)msgQueue->msgCount;
			msgQueue->usedCount = (uint32)msgQueue->usedCount + 1;
			OSMessage* newMsg = &(msgQueue->msgArray[messageIndex]);
			memcpy(newMsg, msg, sizeof(OSMessage));
		}
		if (!s_tracedMessageQueues.empty() && s_tracedMessageQueues.count(msgQueue)) [[unlikely]]
		{
			std::string sender = _MsgTraceCurrentThreadName();
			static std::map<OSMessageQueue*, uint32> s_sendLinesPerQueue; // some queues get many messages per second
			uint32& sendLines = s_sendLinesPerQueue[msgQueue];
			if (sendLines < 200)
			{
				sendLines++;
				cemuLog_log(LogType::Force, "Thread trace: {} sends message {:08x} {:08x} {:08x} {:08x} to queue {:08x} (LR 0x{:08x}, receiver waiting: {})",
				sender, (uint32)msg->message, (uint32)msg->data0, (uint32)msg->data1, (uint32)msg->data2, MEMPTR<OSMessageQueue>(msgQueue).GetMPTR(),
				PPCInterpreter_getCurrentInstance() ? PPCInterpreter_getCurrentInstance()->spr.LR : 0, msgQueue->threadQueueReceive.isEmpty() ? "no" : "yes");
				if (sendLines == 200)
					cemuLog_log(LogType::Force, "Thread trace: 200 sends to queue {:08x} logged, no more are logged for it", MEMPTR<OSMessageQueue>(msgQueue).GetMPTR());
			}
			auto& senders = s_tracedQueueSenders[msgQueue];
			if ((flags & OS_MESSAGE_HIGH_PRIORITY))
				senders.emplace_front(_MsgTraceNowNs(), sender);
			else
				senders.emplace_back(_MsgTraceNowNs(), sender);
			if (senders.size() > 256)
				senders.pop_back();
		}
		// wake up any thread waiting to read a message
		if (!msgQueue->threadQueueReceive.isEmpty())
			msgQueue->threadQueueReceive.wakeupSingleThreadWaitQueue(true);
		__OSUnlockScheduler();
		return 1;
	}

	OSMessageQueue* OSGetSystemMessageQueue()
	{
		return g_systemMessageQueue.GetPtr();
	}

	void InitializeMessageQueue()
	{
		OSInitMessageQueue(g_systemMessageQueue.GetPtr(), _systemMessageQueueArray.GetPtr(), _systemMessageQueueArray.GetCount());

		cafeExportRegister("coreinit", OSInitMessageQueueEx, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSInitMessageQueue, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSReceiveMessage, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSPeekMessage, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSSendMessage, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSGetSystemMessageQueue, LogType::CoreinitThread);
	}
};

