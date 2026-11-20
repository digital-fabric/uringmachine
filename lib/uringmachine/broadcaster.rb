# frozen_string_literal: true

class UringMachine
  # A Broadcaster implements a broadcast pattern where multiple fibers may be
  # notified of an event or some arbitrary value. Listeners listen for events
  # using the #each method.
  class Broadcaster
    # Initializes a broadcaster instance.
    #
    # @param machine [UringMachine]
    # @return [void]
    def initialize(machine)
      @machine = machine
      @listeners = Set.new
    end

    # Returns the number of currently listening fibers.
    #
    # @return [Integer]
    def listener_count
      @listeners.size
    end

    # Broadcasts the given value to all listeners.
    #
    # @param value [any]
    # @return [void]
    def broadcast(value)
      @listeners.each { @machine.push(it, value) }
    end

    # Listens for broadcast values in an infinite loop, yielding each broadcast
    # value to the given block.
    #
    # @return [void]
    def each
      mailbox = Fiber.current.mailbox
      @listeners << mailbox
      loop { yield @machine.shift(mailbox) }
    ensure
      @listeners.delete(mailbox)    
    end
  end
end
