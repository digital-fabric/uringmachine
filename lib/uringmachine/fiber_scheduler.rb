# frozen_string_literal: true

class UringMachine
  class FiberScheduler
    def initialize(machine)
      @machine = machine
    end

    def p(o)
      @machine.write(1, "#{o.inspect}\n")
    rescue Errno::EINTR
      retry
    end

    def join(*)
      @machine.join(*)
    end

    def block(blocker, timeout)
      p block: [blocker, timeout]

    end

    def unblock(blocker, fiber)
      p unblock: [blocker, fiber]
    end

    def kernel_sleep(duration = nil)
      # p sleep: [duration]
      if duration
        @machine.sleep(duration)
      else
        @machine.yield
      end
    end

    def io_wait(io, events, timeout = nil)
      timeout ||= io.timeout
      p timeout: timeout
      if timeout
        p 1
        @machine.timeout(timeout, Timeout::Error) {
          p 2
          @machine.poll(io.fileno, events).tap { p 3 }
      }.tap { p 4 }
      else
        p 5
        @machine.poll(io.fileno, events).tap { p 6 }

      end
    rescue => e
      p e: e
      raise
    end

    def fiber(&block)
      f = @machine.spin(&block)
      @machine.snooze
      f
    end

    def io_write(io, buffer, length, offset)
      p io_write: [io, buffer.get_string, length, offset]
      @machine.write(io.fileno, buffer.get_string)
    end

    def io_read(io, buffer, length, offset)
      # p io_read: [io, buffer, length, offset]
      s = +''
      length = buffer.size if length == 0
      bytes = @machine.read(io.fileno, s, length)
      buffer.set_string(s)
      bytes
    rescue SystemCallError => e
      -e.errno
    end

    def io_pwrite(io, buffer, from, length, offset)
      p io_pwrite: [io, buffer, from, length, offset]
    end

    def io_pread(io, buffer, from, length, offset)
      p io_pread: [io, buffer, from, length, offset]
    end

    # def fiber(&block)
    #   fiber = Fiber.new(blocking: false, &block)
    #   unblock(nil, fiber)
    #   # fiber.resume
    #   return fiber
    # end

    # def kernel_sleep(duration = nil)
    #   block(:sleep, duration)
    # end

    # def process_wait(pid, flags)
    #   # This is a very simple way to implement a non-blocking wait:
    #   Thread.new do
    #     Process::Status.wait(pid, flags)
    #   end.value
    # end
  end
end