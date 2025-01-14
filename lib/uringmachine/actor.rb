# frozen_string_literal: true

class UringMachine
  def spin_actor(mod, *a, **k)
    target = Object.new.extend(mod)
    mailbox = UM::Queue.new
    actor = spin(nil, Actor) { actor.run(self, target, mailbox) }
    target.setup(*a, **k)
    snooze
    actor
  end

  class Actor < Fiber
    def run(machine, target, mailbox)
      @machine = machine
      @target = target      
      @mailbox = mailbox
      while (msg = machine.shift(mailbox))
        process_message(msg)
      end
    ensure
      @target.teardown if @target.respond_to?(:teardown)
    end

    def cast(sym, *a, **k)
      self << [:cast, nil, sym, a, k]
      self
    end

    def call(sym, *a, **k)
      self << [:call, Fiber.current, sym, a, k]
      @machine.yield
    end

    private

    def process_message(msg)
      type, fiber, sym, args, kwargs = msg
      case type
      when :cast
        @target.send(sym, *args, **kwargs)
      when :call
        res = @target.send(sym, *args, **kwargs)
        @machine.schedule(fiber, res)
      end
    end

    def <<(msg)
      @machine.push(@mailbox, msg)
    end
  end
end
