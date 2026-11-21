#pragma once
#include <type_traits>
#include <atomic>
#include <functional>

// Per-process host state. Host (C++) variables that describe the running title register here, so a title can be
// suspended: Save() moves the current value out and resets the variable to the value it had at program start (as if
// no title had run), Restore() moves the saved value back. Guest memory is handled separately by the MMU (swapped by
// remapping). Register a variable once, in the file that defines it, after its definition:
//   CAFE_PROCESS_STATE(s_myVariable);
// Phases: thread state is saved before the scheduler stops (it owns the guest threads' host fibers), everything else
// after the scheduler and the GPU thread stopped
namespace CafeProcess
{
	enum class StatePhase
	{
		Threads, // saved before OSSchedulerEnd, restored right before OSSchedulerBegin
		General, // saved after the scheduler and GPU thread stopped, restored before they start
	};

	struct StateEntry
	{
		StateEntry(const char* name, StatePhase phase);
		virtual ~StateEntry() = default;
		virtual void* Save() = 0; // moves the value out and resets the variable to its initial value
		virtual void Restore(void* saved) = 0; // moves the value back and frees the holder
		virtual void Discard(void* saved) = 0; // frees the holder without restoring
		const char* name;
		StatePhase phase;
	};

	template<typename T>
	struct is_std_atomic : std::false_type {};
	template<typename T>
	struct is_std_atomic<std::atomic<T>> : std::true_type {};

	template<typename T>
	struct StateVar : StateEntry
	{
		StateVar(const char* name, T& var, StatePhase phase = StatePhase::General) : StateEntry(name, phase), m_var(var)
		{
			if constexpr (is_std_atomic<T>::value)
				m_initial = new typename T::value_type(var.load());
			else if constexpr (std::is_trivially_copyable_v<T>)
			{
				m_initial = new uint8[sizeof(T)];
				memcpy(m_initial, (const void*)&var, sizeof(T));
			}
			else if constexpr (std::is_copy_constructible_v<T>)
				m_initial = new T(var);
			else
				m_initial = nullptr; // move-only: initial value is the default constructed value
		}

		void* Save() override
		{
			if constexpr (is_std_atomic<T>::value)
			{
				auto* saved = new typename T::value_type(m_var.load());
				m_var.store(*(typename T::value_type*)m_initial);
				return saved;
			}
			else if constexpr (std::is_trivially_copyable_v<T>)
			{
				uint8* saved = new uint8[sizeof(T)];
				memcpy(saved, (const void*)&m_var, sizeof(T));
				memcpy((void*)&m_var, m_initial, sizeof(T));
				return saved;
			}
			else
			{
				T* saved = new T(std::move(m_var));
				if constexpr (std::is_copy_constructible_v<T>)
					m_var = *(T*)m_initial;
				else
					m_var = T{};
				return saved;
			}
		}

		void Restore(void* saved) override
		{
			if constexpr (is_std_atomic<T>::value)
			{
				m_var.store(*(typename T::value_type*)saved);
				delete (typename T::value_type*)saved;
			}
			else if constexpr (std::is_trivially_copyable_v<T>)
			{
				memcpy((void*)&m_var, saved, sizeof(T));
				delete[] (uint8*)saved;
			}
			else
			{
				m_var = std::move(*(T*)saved);
				delete (T*)saved;
			}
		}

		void Discard(void* saved) override
		{
			if constexpr (is_std_atomic<T>::value)
				delete (typename T::value_type*)saved;
			else if constexpr (std::is_trivially_copyable_v<T>)
				delete[] (uint8*)saved;
			else
				delete (T*)saved;
		}

	  private:
		T& m_var;
		void* m_initial;
	};

	// state that needs custom code (e.g. a counter inside a class)
	struct StateCallbacks : StateEntry
	{
		StateCallbacks(const char* name, StatePhase phase, std::function<void*()> save, std::function<void(void*)> restore, std::function<void(void*)> discard)
			: StateEntry(name, phase), m_save(std::move(save)), m_restore(std::move(restore)), m_discard(std::move(discard)) {}
		void* Save() override { return m_save(); }
		void Restore(void* saved) override { m_restore(saved); }
		void Discard(void* saved) override { m_discard(saved); }
	  private:
		std::function<void*()> m_save;
		std::function<void(void*)> m_restore;
		std::function<void(void*)> m_discard;
	};

	std::vector<StateEntry*>& GetStateRegistry();
}

#define _CAFE_PROCESS_CONCAT2(a, b) a##b
#define _CAFE_PROCESS_CONCAT(a, b) _CAFE_PROCESS_CONCAT2(a, b)
#define CAFE_PROCESS_STATE(var) static CafeProcess::StateVar<std::remove_reference_t<decltype(var)>> _CAFE_PROCESS_CONCAT(_cafeProcessState_, __COUNTER__){#var, var}
#define CAFE_PROCESS_STATE_THREADS(var) static CafeProcess::StateVar<std::remove_reference_t<decltype(var)>> _CAFE_PROCESS_CONCAT(_cafeProcessState_, __COUNTER__){#var, var, CafeProcess::StatePhase::Threads}
