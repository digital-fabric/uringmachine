# frozen_string_literal: true

require 'resolv'
require 'etc'
require 'uringmachine'

class UringMachine
  # Implements a worker thread pool for running blocking operations. Worker
  # threads are started as needed. Worker thread count is limited to the number
  # of CPU cores available.
  class BlockingOperationThreadPool

    # Initializes a new worker pool.
    #
    # @return [void]
    def initialize
      @pending_count = 0
      @worker_count = 0
      @max_workers = Etc.nprocessors
      @worker_mutex = UM::Mutex.new
      @job_queue = UM::Queue.new
      @workers = []
    end

    # Processes a request by submitting it to the job queue and waiting for the
    # return value. Starts a worker if needed.
    #
    # @param machine [UringMachine] machine
    # @param job [any] callable job object
    # @return [any] return value
    def process(machine, job)
      queue = Fiber.current.mailbox
      if @worker_count == 0 || (@pending_count > 0 && @worker_count < @max_workers)
        start_worker(machine)
      end
      machine.push(@job_queue, [queue, job])
      machine.shift(queue)
    end

    private

    # @param machine [UringMachine] machine
    # @return [void]
    def start_worker(machine)
      machine.synchronize(@worker_mutex) do
        return if @worker_count == @max_workers

        @workers << Thread.new { run_worker_thread }
        @worker_count += 1
      end
    end

    # @return [void]
    def run_worker_thread
      machine = UM.new(4).mark(1)
      loop do
        q, op = machine.shift(@job_queue)
        @pending_count += 1
        res = begin
          op.()
        rescue Exception => e
          e
        end
        @pending_count -= 1
        machine.push(q, res)
      rescue => e
        UM.debug("worker e: #{e.inspect}")
      end
    end
  end

  # UringMachine::FiberScheduler implements the Fiber::Scheduler interface for
  # creating fiber-based concurrent applications in Ruby, in tight integration
  # with the standard Ruby I/O and locking APIs.
  class FiberScheduler

    # The blocking operation thread pool is shared by all fiber schedulers.
    @@blocking_operation_thread_pool = BlockingOperationThreadPool.new

    # UringMachine instance associated with scheduler.
    attr_reader :machine

    # WeakMap holding references scheduler fibers as keys.
    attr_reader :fiber_map

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

    # :nodoc:
    def instance_variables_to_inspect
      [:@machine]
    end

    # Creates a new fiber with the given block. The created fiber is added to
    # the fiber map, scheduled on the scheduler machine, and started before this
    # method returns (by calling snooze).
    #
    # @param block [Proc] fiber block @return [Fiber]
    def fiber(&block)
      fiber = Fiber.new(blocking: false) { @machine.run(fiber, &block) }
      @fiber_map[fiber] = true
      @machine.schedule(fiber, nil)
      @machine.snooze
      fiber
    end

    # Waits for all fiber to terminate. Called upon thread termination or when
    # the thread's fiber scheduler is changed.
    #
    # @return [void]
    def scheduler_close
      join()
    end

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

      @machine.wait_fibers(fibers)
    end

    # Runs the given operation in a separate thread, so as not to block other
    # fibers.
    #
    # @param op [callable] blocking operation
    # @return [void]
    def blocking_operation_wait(op)
      @@blocking_operation_thread_pool.process(@machine, op)
    end

    # Blocks the current fiber by yielding to the machine. This hook is called
    # when a synchronization mechanism blocks, e.g. a mutex, a queue, etc.
    #
    # @param blocker [any] blocker object
    # @param timeout [Number, nil] optional timeout
    # @return [bool] was the operation successful
    def block(blocker, timeout = nil)
      if timeout
        @machine.timeout(timeout, Timeout::Error) { @machine.yield }
      else
        @machine.yield
      end
      true
    rescue Timeout::Error
      false
    end

    # Unblocks the given fiber by scheduling it. This hook is
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

    # Sleeps for the given duration.
    #
    # @param duration [Number, nil] sleep duration
    # @return [void]
    def kernel_sleep(duration = nil)
      duration ? @machine.sleep(duration) : @machine.yield
    end

    # Yields to the next runnable fiber.
    def yield
      @machine.snooze
      # @machine.yield
    end

    # Waits for the given io to become ready.
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

    # Selects the first ready IOs from the given sets of IOs.
    #
    # @param rios [Array<IO>] readable IOs
    # @param wios [Array<IO>] writable IOs
    # @param eios [Array<IO>] exceptable IOs
    # @param timeout [Number, nil] optional timeout
    def io_select(rios, wios, eios, timeout = nil)
      map_r = map_fds(rios)
      map_w = map_fds(wios)
      map_e = map_fds(eios)

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

    # Reads from the given IO.
    #
    # @param io [IO] IO object
    # @param buffer [IO::Buffer] read buffer
    # @param length [Integer] read length
    # @param offset [Integer] buffer offset
    # @return [Integer] bytes read
    def io_read(io, buffer, length, offset)
      length = buffer.size if length == 0

      if (timeout = io.timeout)
				@machine.timeout(timeout, Timeout::Error) do
          @machine.read(io.fileno, buffer, length, offset)
        rescue Errno::EINTR
          retry
        end
      else
        @machine.read(io.fileno, buffer, length, offset)
      end
    rescue Errno::EINTR
      retry
    end

    # Reads from the given IO at the given file offset
    #
    # @param io [IO] IO object
    # @param buffer [IO::Buffer] read buffer
    # @param from [Integer] read offset
    # @param length [Integer] read length
    # @param offset [Integer] buffer offset
    # @return [Integer] bytes read
    def io_pread(io, buffer, from, length, offset)
      length = buffer.size if length == 0

      if (timeout = io.timeout)
				@machine.timeout(timeout, Timeout::Error) do
          @machine.read(io.fileno, buffer, length, offset, from)
        rescue Errno::EINTR
          retry
        end
      else
        @machine.read(io.fileno, buffer, length, offset, from)
      end
    rescue Errno::EINTR
      retry
    end

    # Writes to the given IO.
    #
    # @param io [IO] IO object
    # @param buffer [IO::Buffer] write buffer
    # @param length [Integer] write length
    # @param offset [Integer] write offset
    # @return [Integer] bytes written
    def io_write(io, buffer, length, offset)
      # p(io_write: io, length:, offset:, timeout: io.timeout)
      length = buffer.size if length == 0
      buffer = buffer.slice(offset) if offset > 0

      if (timeout = io.timeout)
				@machine.timeout(timeout, Timeout::Error) do
          @machine.write(io.fileno, buffer, length)
        rescue Errno::EINTR
          retry
        end
      else
        @machine.write(io.fileno, buffer, length)
      end
    rescue Errno::EINTR
      retry
    end

    # Writes to the given IO at the given file offset.
    #
    # @param io [IO] IO object
    # @param buffer [IO::Buffer] write buffer
    # @param length [Integer] file offset
    # @param length [Integer] write length
    # @param offset [Integer] buffer offset
    # @return [Integer] bytes written
    def io_pwrite(io, buffer, from, length, offset)
      # p(io_pwrite: io, from:, length:, offset:, timeout: io.timeout)
      length = buffer.size if length == 0
      buffer = buffer.slice(offset) if offset > 0

      if (timeout = io.timeout)
				@machine.timeout(timeout, Timeout::Error) do
          @machine.write(io.fileno, buffer, length, from)
        rescue Errno::EINTR
          retry
        end
      else
        @machine.write(io.fileno, buffer, length, from)
      end
    rescue Errno::EINTR
      retry
    end

    # Closes the given fd.
    #
    # @param fd [Integer] file descriptor
    # @return [Integer] file descriptor
    def io_close(fd)
      @machine.close_async(fd)
    end

    if UM.method_defined?(:waitid_status)

      # Waits for a process to terminate.
      #
      # @param pid [Integer] process pid (0 for any child process)
      # @param flags [Integer] waitpid flags
      # @return [Process::Status] terminated process status
      def process_wait(pid, flags)
        flags = UM::WEXITED if flags == 0
        @machine.waitid_status(UM::P_PID, pid, flags)
      end
    end

    # Interrupts the given fiber with an exception.
    #
    # @param fiber [Fiber] fiber to interrupt
    # @param exception [Exception] Exception
    # @return [void]
    def fiber_interrupt(fiber, exception)
      @machine.schedule(fiber, exception)
      @machine.wakeup
    end

    # Resolves an hostname.
    #
    # @param hostname [String] hostname to resolve
    # @return [Array<Addrinfo>] array of resolved addresses
		def address_resolve(hostname)
			Resolv.getaddresses(hostname)
		end

		# Run the given block with a timeout.
		#
		# @param duration [Number] timeout duration
		# @param exception [Class] exception Class
		# @param message [String] exception message
		# @param block [Proc] block to run
		# @return [any] block return value
    def timeout_after(duration, exception, message, &block)
      @machine.timeout(duration, exception, &block)
    end

    private

    # Prints the given object for debugging purposes.
    #
    # @param o [any]
    # @return [void]
    def p(o) = UM.debug(o.inspect)

    # Maps the given ios to fds.
    #
    # @param ios [Array<IO>] IOs to map
    # @return [Hash] hash mapping fds to IOs
    def map_fds(ios)
      ios.each_with_object({}) { |io, h| h[io.fileno] = io }
    end

    # Maps the given fds to IOs using the given fd-to-IO map.
    #
    # @param fds [Array<Integer>] fds to map
    # @param map [Hash] hash mapping fds to IOs
    # @return [Array<IO>] IOs corresponding to fds
    def unmap_fds(fds, map)
      fds.map { map[it] }
    end
  end
end
