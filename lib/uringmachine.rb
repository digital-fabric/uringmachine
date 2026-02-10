# frozen_string_literal: true

require 'um_ext'
require 'uringmachine/version'
require 'uringmachine/dns_resolver'

UM = UringMachine

# A UringMachine instance provides an interface for performing I/O operations
# and automatically switching between fibers. A single UringMachine instance
# should be used for each thread.
class UringMachine

  # Returns the set of running fibers.
  #
  # return [Set]
  def fiber_set
    @fiber_set ||= Set.new
  end

  # Terminate is an exception used to terminate fibers without further bubbling.
  class Terminate < Exception
  end

  # Creates a new fiber and schedules it to be ran.
  #
  # @param value [any] Value to be passed to the given block
  # @param klass [Class] fiber class
  # @param block [Proc] block to run in fiber
  # @return [Fiber] new fiber
  def spin(value = nil, klass = Fiber, &block)
    fiber = klass.new(blocking: false) { |v| run_block_in_fiber(block, fiber, v) }
    self.schedule(fiber, value)

    fiber_set << fiber
    fiber
  end

  # Runs the given block in the given fiber. This method is used to run fibers
  # indirectly.
  #
  # @param fiber [Fiber] fiber
  # @param block [Proc] block to run
  # @return [Fiber]
  def run(fiber, &block)
    run_block_in_fiber(block, fiber, nil)
    self.schedule(fiber, nil)
    fiber_set << fiber
    fiber
  end

  # Waits for the given fibers to terminate.
  #
  # @return [Array<any>] return values of the given fibers
  def join(*fibers)
    results = fibers.inject({}) { |h, f| h[f] = nil; h }
    queue = nil
    pending = nil
    fibers.each do |f|
      if f.done?
        results[f] = f.result
      else
        (pending ||= []) << f
        queue ||= Fiber.current.mailbox
        f.add_done_listener(queue)
      end
    end
    if pending
      while !pending.empty?
        f = self.shift(queue)
        pending.delete(f)
        results[f] = f.result
      end
    end
    values = results.values
    fibers.size == 1 ? values.first : values
  end

  # Waits for the given fibers to terminate, without collecting their return
  # values.
  #
  # @param fibers [Fiber, Array<Fiber>] fibers to wait for
  # @return [Integer] number of fibers
  def await_fibers(fibers)
    if fibers.is_a?(Fiber)
      f = fibers
      if !f.done?
        queue = Fiber.current.mailbox
        f.add_done_listener(queue)
        self.shift(queue)
      end
      return 1
    end

    queue = nil
    pending = nil
    fibers.each do |f|
      if !f.done?
        (pending ||= []) << f
        queue ||= Fiber.current.mailbox
        f.add_done_listener(queue)
      end
    end
    if pending
      while !pending.empty?
        f = self.shift(queue)
        pending.delete(f)
      end
    end
    fibers.count
  end

  # Resolves a hostname to an IP address by performing a DNS query.
  #
  # @param hostname [String] hostname
  # @param type [Symbol] record type
  # @return [String] IP address
  def resolve(hostname, type = :A)
    @resolver ||= DNSResolver.new(self)
    @resolver.resolve(hostname, type)
  end

  # Watches for filesystem events using inotify in an infinite loop, yielding
  # incoming events to the given block.
  #
  # @param root [String] directory to watch
  # @param mask [Integer] event mask
  # @return [void]
  def file_watch(root, mask)
    fd = UM.inotify_init
    wd_map = {}
    recursive_file_watch(fd, root, wd_map, mask)
    while true
      events = inotify_get_events(fd)
      events.each do |event|
        if event[:mask] | UM::IN_IGNORED == UM::IN_IGNORED
          wd_map.delete(event[:wd])
          next
        end
        transformed_event = transform_file_watch_event(event, wd_map)
        if event[:mask] == UM::IN_CREATE | UM::IN_ISDIR
          recursive_file_watch(fd, transformed_event[:fn], wd_map, mask)
        end
        yield transformed_event
      end
    end
  ensure
    close_async(fd)
  end

  private

  # @param block [Proc]
  # @param fiber [Fiber]
  # @param value [any]
  # @return [void]
  def run_block_in_fiber(block, fiber, value)
    ret = block.(value)
    fiber.set_result(ret)
  rescue Exception => e
    fiber.set_result(e)
  ensure
    fiber.mark_as_done
    # cleanup
    fiber_set.delete(fiber)
    self.notify_done_listeners(fiber)

    # switch away to a different fiber
    self.switch
  end

  # @param fiber [Fiber]
  # @return [void]
  def notify_done_listeners(fiber)
    listeners = fiber.done_listeners
    return if !listeners

    listeners.each { self.push(it, fiber) }
  end

  # @param fd [Integer] inotify fd
  # @param dir [String] directory path
  # @param wd_map [Hash] hash mapping wd to directory
  # @param mask [Integer] inotify event mask
  # @return [void]
  def recursive_file_watch(fd, dir, wd_map, mask)
    wd = UM.inotify_add_watch(fd, dir, mask)
    wd_map[wd] = dir
    Dir[File.join(dir, '*')].each do
      recursive_file_watch(fd, it, wd_map, mask) if File.directory?(it)
    end
  end

  # @param event [Hash] inotify event
  # @param wd_map [Hash] hash mapping wd to directory
  def transform_file_watch_event(event, wd_map)
    {
      mask: event[:mask],
      fn: File.join(wd_map[event[:wd]], event[:name])
    }
  end

  # Fiber extensions
  module FiberExtensions
    attr_reader :result, :done, :done_listeners

    # Marks the fiber as done (terminated)
    def mark_as_done
      @done = true
    end

    # Sets the fiber return value
    def set_result(value)
      @result = value
    end

    # Returns true if the fiber is done (terminated)
    def done?
      @done
    end

    # Adds the given queue to the list of done listeners
    def add_done_listener(queue)
      (@done_listeners ||= []) << queue
    end

    # Returns the fiber's associated mailbox
    def mailbox
      @mailbox ||= UM::Queue.new
    end
  end

  class ::Fiber
    include FiberExtensions
  end

  # Thread extensions
  module ThreadExtensions

    # Returns the thread's associated UringMachine instance
    def machine
      @machine ||= UM.new
    end
  end

  class ::Thread
    include ThreadExtensions
  end
end
