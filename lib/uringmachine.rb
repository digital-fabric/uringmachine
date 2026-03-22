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

  TERMINATE_EXCEPTION = UM::Terminate.new

  # Terminates the given fibers by scheduling them with a `UM::Terminate`
  # exception. This method does not wait for the fibers to be done.
  #
  # @param *fibers [Array<Fiber>] fibers to terminate
  # @return [void]
  def terminate(*fibers)
    fibers = fibers.first if fibers.size == 1 && fibers.first.is_a?(Enumerable)

    fibers.each { schedule(it, TERMINATE_EXCEPTION) unless it.done? }
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

  # Waits for the given fibers to terminate, returning the return value for each
  # given fiber. This method also accepts procs instead of fibers. When a proc
  # is given, it is ran in a separate fiber which will be joined.
  #
  #     machine.join(
  #       -> { machine.sleep(0.01); :f1 },
  #       -> { machine.sleep(0.02); :f2 },
  #       -> { machine.sleep(0.03); :f3 }
  #     ) #=> [:f1, :f2, :f3]
  #
  # @param *fibers [Array<Fiber, Proc>] fibers @return [Array<any>] return
  # values of the given fibers
  def join(*fibers)
    queue = Fiber.current.mailbox

    if fibers.size == 1
      first = fibers.first
      case first
      when Enumerable
        fibers = first
      when Fiber
        first = proc_spin(first) if first.is_a?(Proc)
        if !first.done?
          first.add_done_listener(queue)
          self.shift(queue)
        end
        return first.result
      end
    end

    results = {}
    pending = nil
    fibers.each do |f|
      f = proc_spin(f) if f.is_a?(Proc)
      if f.done?
        results[f] = f.result
      else
        results[f] = nil
        (pending ||= []) << f
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
  def await(*fibers)
    queue = Fiber.current.mailbox

    if fibers.size == 1
      first = fibers.first
      case first
      when Enumerable
        fibers = first
      when Fiber
        first = proc_spin(first) if first.is_a?(Proc)
        if !first.done?
          first.add_done_listener(queue)
          self.shift(queue)
        end
        return 1
      end
    end

    pending = nil
    fibers.each do |f|
      f = proc_spin(f) if f.is_a?(Proc)
      if !f.done?
        (pending ||= []) << f
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

  # call-seq:
  #   machine.stream(fd, mode = nil) -> stream
  #   machine.stream(fd, mode = nil) { |stream| }
  #
  # Creates a stream for reading from the given target fd or other object. The
  # mode indicates the type of target and how it is read from:
  #
  # - :bp_read - read from the given fd using the buffer pool (default mode)
  # - :bp_recv - receive from the given socket fd using the buffer pool
  # - :ssl - read from the given SSL connection
  #
  # If a block is given, the block will be called with the stream object and the
  # method will return the block's return value.
  #
  # @param target [Integer, OpenSSL::SSL::SSLSocket] fd or ssl connection
  # @param mode [Symbol, nil] stream mode
  # @return [UringMachine::Stream] stream object
  def stream(target, mode = nil)
    stream = UM::Stream.new(self, target, mode)
    return stream if !block_given?

    res = yield(stream)
    stream.clear
    res
  end

  # Creates, binds and sets up a TCP socket for listening on the given host and
  # port.
  #
  # @param host [String] host IP address
  # @param port [Integer] TCP port
  # @return [Integer] socket fd
  def tcp_listen(host, port)
    fd = socket(UM::AF_INET, UM::SOCK_STREAM, 0, 0)
    bind(fd, host, port)
    listen(fd, UM::SOMAXCONN)
    fd
  end

  # Creates and connects a TCP socket to the given host and port.
  #
  # @param host [String] host IP address
  # @param port [Integer] TCP port
  # @return [Integer] socket fd
  def tcp_connect(host, port)
    fd = socket(UM::AF_INET, UM::SOCK_STREAM, 0, 0)
    connect(fd, host, port)
    fd
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
    fiber_set.delete(fiber)
    self.notify_done_listeners(fiber)

    # finally switch away to a different fiber, the current fiber should not be
    # resumed after this.
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

  # @param proc [Proc]
  # @return [Fiber]
  def proc_spin(proc)
    if proc.arity == 0
      spin { proc.call }
    else
      spin(&proc)
    end
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
