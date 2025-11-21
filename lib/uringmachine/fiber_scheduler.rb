# frozen_string_literal: true

class UringMachine
  class FiberScheduler
    def initialize(machine)
      @machine = machine
      @ios = ObjectSpace::WeakMap.new
      @running_fibers = ObjectSpace::WeakMap.new
    end

    def method_missing(sym, *a, **b)
      @machine.write(1, "method_missing: #{sym.inspect} #{a.inspect} #{b.inspect}\n")
      @machine.write(1, "#{caller.inspect}\n")
    end

    def scheduler_close
      fibers = @running_fibers.keys
      # p(fiber_count: fibers.size)
      @machine.join(*fibers)
      @machine.write(1, "scheduler done\n");
    end

    def fiber_interrupt(fiber, exception)
      # p(fiber_interrput: fiber, exception:)
    end

    def p(o) = @machine.write(1, "#{o.inspect}\n")

    def join(*fibers)
      if fibers.empty?
        fibers = @running_fibers.keys
        # p(join: fibers, done: fibers.map(&:done?))
        @running_fibers = ObjectSpace::WeakMap.new
      end

      @machine.join(*fibers)
    end

    def block(blocker, timeout = nil)
      @machine.yield
    end

    def unblock(blocker, fiber)
      @machine.schedule(fiber, nil)
    end

    def kernel_sleep(duration = nil)
      if duration
        @machine.sleep(duration)
      else
        @machine.yield
      end
    end

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

    def fiber(&block)
      f = Fiber.new(blocking: false) { @machine.run(f, &block) }
      @running_fibers[f] = true
      @machine.schedule(f, nil)
      @machine.snooze
      # p(spin: f)
      f
    end

    def io_write(io, buffer, length, offset)
      reset_nonblock(io)
      @machine.write(io.fileno, buffer.get_string)
    rescue Errno::EINTR
      retry
    end

    def io_read(io, buffer, length, offset)
      reset_nonblock(io)
      s = +''
      length = buffer.size if length == 0
      bytes = @machine.read(io.fileno, s, length)
      buffer.set_string(s)
      bytes
    rescue Errno::EINTR
      retry
    end

    private

    def reset_nonblock(io)
      return if @ios.key?(io)
        
      @ios[io] = true
      UM.io_set_nonblock(io, false)  
    end
  end
end