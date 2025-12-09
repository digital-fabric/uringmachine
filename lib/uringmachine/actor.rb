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

  def spin_thread_actor(mod, *a, **k)
    machine = UM.new
    target = Object.new.extend(mod)
    mailbox = UM::Queue.new
    actor = Actor.new
    Thread.new do
      actor.run(machine, target, mailbox)
    end
    target.setup(*a, **k)
    snooze
    actor
  end

  class Actor < Fiber
    class Stop < UM::Error; end

    def run(machine, target, mailbox)
      @machine = machine
      @target = target
      @mailbox = mailbox
      while (msg = machine.shift(mailbox))
        process_message(msg)
      end
    rescue Stop
      # stopped
    ensure
      @target.teardown if @target.respond_to?(:teardown)
    end

    def cast(sym, *a, **k)
      @machine.push @mailbox, [:cast, nil, sym, a, k]
      self
    end

    def call(response_mailbox, sym, *a, **k)
      @machine.push @mailbox, [:call, response_mailbox, sym, a, k]
      @machine.shift response_mailbox
    end

    def stop
      @machine.schedule(self, Stop.new)
    end

    private

    def process_message(msg)
      type, response_mailbox, sym, args, kwargs = msg
      case type
      when :cast
        @target.send(sym, *args, **kwargs)
      when :call
        res = @target.send(sym, *args, **kwargs)
        @machine.push(response_mailbox, res)
      end
    end
  end
end
