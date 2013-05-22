// CC0 Public Domain: http://creativecommons.org/publicdomain/zero/1.0/
#pragma once

#include <functional>
#include <vector>
#include <assert.h>

namespace Signal11 {

	class ConnectionRef;

	namespace Lib {

		/// ProtoSignal is the template implementation for callback list.
		template<typename,typename> class ProtoSignal;   // undefined

		/// CollectorInvocation invokes signal handlers differently depending on return type.
		template<typename,typename> class CollectorInvocation;

		/// CollectorLast returns the result of the last signal handler from a signal emission.
		template<typename Result>
		class CollectorLast {
		public:
			typedef Result CollectorResult;

			explicit CollectorLast()
			{
			}

			inline bool operator()(Result r) {
				_last = r;
				return true;
			}
			
			CollectorResult result() {
				return _last;
			}

		private:
			Result _last;
		};

		/// CollectorDefault implements the default signal handler collection behaviour.
		template<typename Result>
		class CollectorDefault : CollectorLast<Result>
		{};

		/// CollectorDefault specialisation for signals with void return type.
		template<>
		class CollectorDefault<void> {
		public:
			typedef void CollectorResult;

			void result() {}
			inline bool operator() (void) {
				return true;
			}
		};

		/// CollectorInvocation specialisation for regular signals.
		template<class Collector, class R, class... Args>
		class CollectorInvocation<Collector, R (Args...)> {
		public:
			inline bool	invoke(Collector &collector, const std::function<R (Args...)> &callback, Args... args) {
				return collector(callback(args...));
			}
		};

		/// CollectorInvocation specialisation for signals with void return type.
		template<class Collector, class... Args>
		struct CollectorInvocation<Collector, void (Args...)> {
			inline bool invoke(Collector &collector, const std::function<void (Args...)> &callback, Args... args) {
				callback(args...);
				return collector();
			}
		};

		struct ProtoSignalLink {
			virtual bool remove() = 0;

			bool isEnabled() const {
				return _enabled;
			}

			void enable() {
				setEnabled(true);
			}

			void disable() {
				setEnabled(false);
			}

			void setEnabled(bool flag) {
				_enabled = flag;
			}

		protected:
			bool _enabled;
		};
	} // namespace Lib

	class ConnectionRef {
	public:
		ConnectionRef(Lib::ProtoSignalLink *link)
			:_link(link)
		{
			assert(link != nullptr);
		}

		ConnectionRef(ConnectionRef &&other)
			:_link(std::move(other._link))
		{
			other._link = nullptr;
		}

		ConnectionRef& operator=(ConnectionRef &&other) {
			std::swap(_link, other._link);

			return *this;
		}

		bool disconnect() {
			if(_link != nullptr) {
				Lib::ProtoSignalLink *link = _link;
				_link = nullptr;
				
				return link->remove();
			}

			return false;
		}

		bool isEnabled() const {
			return _link->isEnabled();
		}

		void enable() {
			_link->enable();
		}

		void disable() {
			_link->disable();
		}

		void setEnabled(bool flag) {
			_link->setEnabled(flag);
		}

	private:
		Lib::ProtoSignalLink *_link;
	};

	class ScopedConnectionRef : public ConnectionRef {
	public:
		ScopedConnectionRef(const ConnectionRef &ref)
			:ConnectionRef(ref)
		{
		}

		ScopedConnectionRef(ConnectionRef &&ref)
			:ConnectionRef(std::move(ref))
		{
		}

		ScopedConnectionRef(ScopedConnectionRef &&other)
			:ConnectionRef(std::move(other))
		{
		}

		~ScopedConnectionRef() {
			disconnect();
		}

		ScopedConnectionRef& operator=(ScopedConnectionRef &&other) {
			ConnectionRef::operator=(std::move(other));

			return *this;
		}
		
	private:
		ScopedConnectionRef(const ScopedConnectionRef &other)
			:ConnectionRef(nullptr)
		{}
		ScopedConnectionRef& operator=(const ScopedConnectionRef &other) { return *this; }
	};
	
	class ConnectionScope {
	public:
		ConnectionRef& operator+=(const ConnectionRef &ref) {
			_connections.push_back(ref);

			return _connections.back();
		}

		ConnectionRef& operator+=(ConnectionRef &&ref) {
			_connections.push_back(std::move(ref));

			return _connections.back();
		}

		ConnectionRef& operator+=(ScopedConnectionRef &&ref) {
			_connections.push_back(std::move(ref));

			return _connections.back();
		}

	private:
		std::vector<ScopedConnectionRef> _connections;
	};

	namespace Lib {
		/// ProtoSignal template specialised for the callback signature and collector.
		template<class Collector, class R, class... Args>
		class ProtoSignal<R (Args...), Collector> : private CollectorInvocation<Collector, R (Args...)> {
		protected:
			typedef std::function<R (Args...)> CallbackFunction;
			typedef typename CallbackFunction::result_type Result;
			typedef typename Collector::CollectorResult CollectorResult;

		private:
			/// SignalLink implements a doubly-linked ring with ref-counted nodes containing the signal handlers.
			struct SignalLink : public ProtoSignalLink {
				SignalLink *_next;
				SignalLink *_prev;
				CallbackFunction _callbackFunc;
				int _refCount;

				friend class ConnectionRef;

				explicit SignalLink(const CallbackFunction &callback)
					:_next(nullptr), _prev(nullptr), _callbackFunc(callback), _refCount(1)
				{
				}

				~SignalLink() {
					assert(_refCount == 0);
				}

				void incref() {
					_refCount += 1;
					assert(_refCount > 0);
				}

				void decref() {
					_refCount -= 1;
					
					if(_refCount == 0) {
						delete this;
					} else {
						assert(_refCount > 0);
					}
				}

				void unlink() {
					_callbackFunc = nullptr;
					
					if(_next) {
						_next->_prev = _prev;
					}

					if(_prev) {
						_prev->_next = _next;
					}

					decref();
					// leave intact ->next, ->prev for stale iterators
				}

				SignalLink* addBefore(const CallbackFunction &callback) {
					SignalLink *link = new SignalLink(callback);
					link->_prev = _prev; // link to last
					link->_next = this;
					_prev->_next = link; // link from last
					_prev = link;

					static_assert(sizeof(link) == sizeof(size_t), "sizeof size_t");

					return link;
				}

				bool deactivate(const CallbackFunction &callback) {
					if (callback == _callbackFunc) {
						_callbackFunc = nullptr;      // deactivate static head
						return true;
					}

					SignalLink *link = this->next ? this->next : this;

					for(; link != this; link = link->next) {
						if (callback == link->_callbackFunc) {
							link->unlink();     // deactivate and unlink sibling
							return true;
						}
					}

					return false;
				}

				bool removeSibling(SignalLink *id) {
					SignalLink *link = this->_next ? this->_next : this;

					for(; link != this; link = link->_next) {
						if(id == link) {
							link->unlink();     // deactivate and unlink sibling
							return true;
						}
					}
					
					return false;
				}

				bool remove() {
					if(!this->_next) {
						return false;
					}

					return this->_next->removeSibling(this);
				}
			};

			SignalLink *_callbackRing; // linked ring of callback nodes
			
			void ensureRing() {
				if(!_callbackRing) {
					_callbackRing = new SignalLink(CallbackFunction()); // refCount = 1
					_callbackRing->incref(); // refCount = 2, head of ring, can be deactivated but not removed
					_callbackRing->_next = _callbackRing; // ring head initialization
					_callbackRing->_prev = _callbackRing; // ring tail initialization
				}
			}

			ProtoSignal (const ProtoSignal&) {}
			ProtoSignal& operator=(const ProtoSignal&) {}

		public:
			/// ProtoSignal constructor, connects default callback if non-NULL.
			ProtoSignal(const CallbackFunction &method)
				:_callbackRing(nullptr)
			{
				if(method != nullptr) {
					ensureRing();
					_callbackRing->_callbackFunc = method;
				}
			}

			ProtoSignal(ProtoSignal &&other)
				:_callbackRing(std::move(other._callbackRing))
			{
				other._callbackRing = nullptr;
			}

			/// ProtoSignal destructor releases all resources associated with this signal.
			~ProtoSignal() {
				if (_callbackRing) {
					while(_callbackRing->_next != _callbackRing) {
						_callbackRing->_next->unlink();
					}

					assert(_callbackRing->_refCount >= 2);
					_callbackRing->decref();
					_callbackRing->decref();
				}
			}

			ProtoSignal& operator=(ProtoSignal &&other) {
				std::swap(_callbackRing, other._callbackRing);

				return *this;
			}

			/// Operator to add a new function or lambda as signal handler, returns a handler connection ID.
			ConnectionRef connect(const CallbackFunction &callback) {
				ensureRing();
				return ConnectionRef(_callbackRing->addBefore(callback));
			}

			template<class T>
			ConnectionRef connect(T &object, R(T::*method)(Args...)) {
				CallbackFunction wrapper = [&object, method](Args... args) {
					return (object.*method)(std::forward<Args>(args)...);
				};

				return connect(wrapper);
			}

			template<class T>
			ConnectionRef connect(T *object, R(T::*method)(Args...)) {
				CallbackFunction wrapper = [&object, method](Args... args) {
					return (object->*method)(std::forward<Args>(args)...);
				};

				return connect(wrapper);
			}

			/// Operator to remove a signal handler through it connection ID, returns if a handler was removed.
			bool disconnect(ConnectionRef &connectionRef) {
				return connectionRef.disconnect();
			}

			bool disconnect(SignalLink *link) {
				return _callbackRing ? _callbackRing->removeSibling(link) : false;
			}

			bool disconnect(ProtoSignalLink *link) {
				return disconnect((SignalLink*)link);
			}

			/// Emit a signal, i.e. invoke all its callbacks and collect return types with the Collector.
			CollectorResult emit(Args... args) {
				Collector collector;

				if (!_callbackRing) {
					return collector.result();
				}

				SignalLink *link = _callbackRing;
				link->incref();

				do {
					if(link->isEnabled() && link->_callbackFunc != nullptr) {
						const bool continueEmission = this->invoke(collector, link->_callbackFunc, args...);

						if(!continueEmission) {
							break;
						}
					}

					SignalLink *old = link;
					link = old->_next;
					link->incref();
					old->decref();
				}

				while (link != _callbackRing);
				
				link->decref();
				return collector.result();
			}
		};

	} // Lib
	// namespace Signal11

	/**
	* Signal is a template type providing an interface for arbitrary callback lists.
	* A signal type needs to be declared with the function signature of its callbacks,
	* and optionally a return result collector class type.
	* Signal callbacks can be added with operator+= to a signal and removed with operator-=, using
	* a callback connection ID return by operator+= as argument.
	* The callbacks of a signal are invoked with the emit() method and arguments according to the signature.
	* The result returned by emit() depends on the signal collector class. By default, the result of
	* the last callback is returned from emit(). Collectors can be implemented to accumulate callback
	* results or to halt a running emissions in correspondance to callback results.
	* The signal implementation is safe against recursion, so callbacks may be removed and
	* added during a signal emission and recursive emit() calls are also safe.
	* The overhead of an unused signal is intentionally kept very low, around the size of a single pointer.
	* Note that the Signal template types is non-copyable.
	*/
	template <typename SignalSignature, class Collector = Lib::CollectorDefault<typename std::function<SignalSignature>::result_type> >
	struct Signal final : Lib::ProtoSignal<SignalSignature, Collector> {
		typedef Lib::ProtoSignal<SignalSignature, Collector> ProtoSignal;
		typedef typename ProtoSignal::CallbackFunction CallbackFunction;

		/// Signal constructor, supports a default callback as argument.
		Signal(const CallbackFunction &method = CallbackFunction())
			:ProtoSignal(method)
		{
		}

		Signal(Signal &&other)
			:ProtoSignal(std::move(other))
		{
		}

		Signal& operator=(Signal &&other) {
			ProtoSignal::operator=(std::move(other));

			return *this;
		}
	};

	/// Keep signal emissions going while all handlers return !0 (true).
	template<typename Result>
	struct CollectorUntil0 {
		typedef Result CollectorResult;

		explicit CollectorUntil0()
			:_result()
		{
		}

		const CollectorResult& result() {
			return _result;
		}

		inline bool	operator() (Result r) {
			_result = r;
			return _result ? true : false;
		}

	private:
		CollectorResult _result;
	};

	/// Keep signal emissions going while all handlers return 0 (false).
	template<typename Result>
	struct CollectorWhile0 {
		typedef Result CollectorResult;

		explicit CollectorWhile0()
			:_result()
		{
		}

		const CollectorResult& result() {
			return _result;
		}

		inline bool operator() (Result r) {
			_result = r;
			return _result ? false : true;
		}

	private:
		CollectorResult _result;
	};

	/// CollectorVector returns the result of the all signal handlers from a signal emission in a std::vector.
	template<typename Result>
	struct CollectorVector {
		typedef std::vector<Result> CollectorResult;

		const CollectorResult& result() {
			return _result;
		}

		inline bool	operator() (Result r) {
			_result.push_back (r);
			return true;
		}

	private:
		CollectorResult _result;
	};

} // Signal11