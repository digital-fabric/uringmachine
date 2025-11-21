# frozen_string_literal: true

class UringMachine
  # UringMachine::FiberScheduler implements the Fiber::Scheduler interface for
  # creating fiber-based concurrent applications in Ruby, in tight integration
  # with the standard Ruby I/O and locking APIs.
  class FiberScheduler
    attr_reader :fiber_map

    # Instantiates a scheduler with the given UringMachine instance.
    #
    #     machine = UM.new
    #     scheduler = UM::FiberScheduler.new(machine)
    #     Fiber.set_scheduler(scheduler)
    #
    # @param machine [UringMachine] associated UringMachine instance
    # @return [void]
    def initialize(machine)
      @machine = machine
      @ios = ObjectSpace::WeakMap.new
      @fiber_map = ObjectSpace::WeakMap.new
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

    # fiber_interrupt hook: to be implemented.
    def fiber_interrupt(fiber, exception)
      raise NotImplementedError, "Implement me!"
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
      start_blocking_operation_thread

      queue = UM::Queue.new
      @machine.push(@blocking_op_queue, [queue, blocking_operation])
      @machine.shift(queue)

      # UM.debug("block_operation_wait #{Fiber.current.inspect} >>")
      # UM.debug("  #{blocking_operation.inspect}")
      # Thread.new do
      #   UM.debug("th >>"); blocking_operation.(); UM.debug("th <<")
      # end.join
      # UM.debug("block_operation_wait <<")
    end

    def start_blocking_operation_thread
      @blocking_op_queue ||= UM::Queue.new      
      @blocking_op_thread ||= Thread.new do
        m = UM.new
        loop do
          q, op = m.shift(@blocking_op_queue)
          res = begin
            op.()
          rescue Exception => e
            e
          end
          m.push(q, res)
        end
      end
    end

    # block hook: blocks the current fiber by yielding to the machine. This hook
    # is called when a synchronization mechanism blocks, e.g. a mutex, a queue,
    # etc.
    #
    # @param blocker [any] blocker object
    # @param timeout [Number, nil] optional
    # timeout @return [void]
    def block(blocker, timeout = nil)
      raise NotImplementedError, "Implement me!" if timeout

      @machine.yield
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
          @machine.poll(io.fileno, events).tap { p 3 }
        }
      else
        @machine.poll(io.fileno, events).tap { p 6 }

      end
    rescue => e
      p e: e
      raise
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
      ensure_nonblock(io)
      @machine.write(io.fileno, buffer.get_string)
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
      ensure_nonblock(io)
      s = +''
      length = buffer.size if length == 0
      bytes = @machine.read(io.fileno, s, length)
      buffer.set_string(s)
      bytes
    rescue Errno::EINTR
      retry
    end

    private

    # Ensures the given IO is in blocking mode.
    #
    # @param io [IO] IO object
    # @return [void]
    def ensure_nonblock(io)
      return if @ios.key?(io)
        
      @ios[io] = true
      UM.io_set_nonblock(io, false)  
    end
  end
end