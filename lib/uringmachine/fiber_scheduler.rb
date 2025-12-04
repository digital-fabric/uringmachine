# frozen_string_literal: true

require 'resolv'
require 'etc'

class UringMachine
  # Implements a thread pool for running blocking operations. 
  class BlockingOperationThreadPool
    def initialize
      @blocking_op_queue = UM::Queue.new
      @pending_count = 0
      @worker_count = 0
      @max_workers = Etc.nprocessors
      @worker_mutex = UM::Mutex.new
      @workers = []
    end

    def process(machine, job)
      queue = UM::Queue.new

      if @worker_count == 0 || (@pending_count > 0 && @worker_count < @max_workers)
        start_worker(machine)
      end 
      machine.push(@blocking_op_queue, [queue, job])
      machine.shift(queue)
    end

    private
    
    def start_worker(machine)
      machine.synchronize(@worker_mutex) do
        return if @worker_count == @max_workers

        @workers << Thread.new { run_worker_thread }
        @worker_count += 1
      end
    end

    def run_worker_thread
      machine = UM.new(4)
      loop do
        q, op = machine.shift(@blocking_op_queue)
        @pending_count += 1
        res = begin
          op.()
        rescue Exception => e
          e
        end
        @pending_count -= 1
        machine.push(q, res)
      end
    end
  end

  # UringMachine::FiberScheduler implements the Fiber::Scheduler interface for
  # creating fiber-based concurrent applications in Ruby, in tight integration
  # with the standard Ruby I/O and locking APIs.
  class FiberScheduler
    @@blocking_operation_thread_pool = BlockingOperationThreadPool.new

    attr_reader :machine, :fiber_map

    # Instantiates a scheduler with the given UringMachine instance.
    #
    #     machine = UM.new
    #     scheduler = UM::FiberScheduler.new(machine)
    #     Fiber.set_scheduler(scheduler)
    #
    # @param machine [UringMachine, nil] UringMachine instance
    # @return [void]
    def initialize(machine = nil)
      @machine = machine || UM.new
      @fiber_map = ObjectSpace::WeakMap.new
    end

    def instance_variables_to_inspect
      [:@machine]
    end

    # Should be called after a fork (eventually, we'll want Ruby to call this
    # automatically after a fork).
    #
    # @return [self]
    def process_fork
      @machine = UM.new
      @fiber_map = ObjectSpace::WeakMap.new
      self
    end

    # For debugging purposes
    def method_missing(sym, *a, **b)
      @machine.write(1, "method_missing: #{sym.inspect} #{a.inspect} #{b.inspect}\n")
      @machine.write(1, "#{caller.inspect}\n")
      super
    end

    # scheduler_close hook: Waits for all fiber to terminate. Called upon thread
    # termination or when the thread's fiber scheduler is changed.
    #
    # @return [void]
    def scheduler_close
      join()
    end

    # For debugging purposes
    def p(o) = UM.debug(o.inspect)

    # Waits for the given fibers to terminate. If no fibers are given, waits for
    # all fibers to terminate.
    #
    # @param fibers [Array<Fiber>] fibers to terminate
    # @return [void]
    def join(*fibers)
      if fibers.empty?
        fibers = @fiber_map.keys
        @fiber_map = ObjectSpace::WeakMap.new
      end

      @machine.join(*fibers)
    end

    # blocking_operation_wait hook: runs the given operation in a separate
    # thread, so as not to block other fibers.
    #
    # @param blocking_operation [callable] blocking operation
    # @return [void]
    def blocking_operation_wait(blocking_operation)
      @@blocking_operation_thread_pool.process(@machine, blocking_operation)
    end

    # block hook: blocks the current fiber by yielding to the machine. This hook
    # is called when a synchronization mechanism blocks, e.g. a mutex, a queue,
    # etc.
    #
    # @param blocker [any] blocker object
    # @param timeout [Number, nil] optional
    # timeout @return [bool] 
    def block(blocker, timeout = nil)
      raise NotImplementedError, "Implement me!" if timeout

      @machine.yield
      true
    end

    # unblock hook: unblocks the given fiber by scheduling it. This hook is
    # called when a synchronization mechanism unblocks, e.g. a mutex, a queue,
    # etc.
    #
    # @param blocker [any] blocker object
    # @param fiber [Fiber] fiber to resume
    # @return [void]
    def unblock(blocker, fiber)
      @machine.schedule(fiber, nil)
      @machine.wakeup
    end

    # kernel_sleep hook: sleeps for the given duration.
    #
    # @param duration [Number, nil] sleep duration
    # @return [void]
    def kernel_sleep(duration = nil)
      if duration
        @machine.sleep(duration)
      else
        @machine.yield
      end
    end

    # io_wait hook: waits for the given io to become ready.
    #
    # @param io [IO] IO object
    # @param events [Number] readiness bitmask
    # @param timeout [Number, nil] optional timeout
    # @param return
    def io_wait(io, events, timeout = nil)
      timeout ||= io.timeout
      if timeout
        @machine.timeout(timeout, Timeout::Error) {
          @machine.poll(io.fileno, events)
        }
      else
        @machine.poll(io.fileno, events)
      end
    end

    def io_select(rios, wios, eios, timeout = nil)
      map_r = map_io_fds(rios)
      map_w = map_io_fds(wios)
      map_e = map_io_fds(eios)

      r, w, e = nil
      if timeout
        @machine.timeout(timeout, Timeout::Error) {
          r, w, e = @machine.select(map_r.keys, map_w.keys, map_e.keys)
        }
      else
        r, w, e = @machine.select(map_r.keys, map_w.keys, map_e.keys)
      end

      [unmap_fds(r, map_r), unmap_fds(w, map_w), unmap_fds(e, map_e)]
    end

    # fiber hook: creates a new fiber with the given block. The created fiber is
    # added to the fiber map, scheduled on the scheduler machine, and started
    # before this method returns (by calling snooze).
    #
    # @param block [Proc] fiber block @return [Fiber]
    def fiber(&block)
      fiber = Fiber.new(blocking: false) { @machine.run(fiber, &block) }
      @fiber_map[fiber] = true
      @machine.schedule(fiber, nil)
      @machine.snooze
      fiber
    end

    # io_write hook: writes to the given IO.
    #
    # @param io [IO] IO object
    # @param buffer [IO::Buffer] write buffer
    # @param length [Integer] write length
    # @param offset [Integer] write offset
    # @return [Integer] bytes written
    def io_write(io, buffer, length, offset)
      if offset > 0
        raise NotImplementedError, "UringMachine currently does not support writing at an offset"
      end

      @machine.write(io.fileno, buffer)
    rescue Errno::EINTR
      retry
    end

    # io_read hook: reads from the given IO.
    #
    # @param io [IO] IO object
    # @param buffer [IO::Buffer] read buffer
    # @param length [Integer] read length
    # @param offset [Integer] read offset
    # @return [Integer] bytes read
    def io_read(io, buffer, length, offset)
      if offset > 0
        raise NotImplementedError, "UringMachine currently does not support reading at an offset"
      end

      length = buffer.size if length == 0
      @machine.read(io.fileno, buffer, length)
    rescue Errno::EINTR
      retry
    end

    if UM.method_defined?(:waitid_status)
      def process_wait(pid, flags)
        flags = UM::WEXITED if flags == 0
        @machine.waitid_status(UM::P_PID, pid, flags)
      end
    end

    def fiber_interrupt(fiber, exception)
      @machine.schedule(fiber, exception)
      @machine.wakeup
    end

		def address_resolve(hostname)
			Resolv.getaddresses(hostname)
		end

    def timeout_after(duration, exception, message, &block)
      @machine.timeout(duration, exception, &block)
    end

    private

    def map_io_fds(ios)
      ios.each_with_object({}) { |io, h| h[io.fileno] = io }
    end

    def unmap_fds(fds, map)
      fds.map { map[it] }
    end

  end
end