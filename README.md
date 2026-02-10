<h1 align="center">
  <br>
  UringMachine
</h1>

<h4 align="center">Ruby on io_uring!</h4>

<p align="center">
  <a href="http://rubygems.org/gems/uringmachine">
    <img src="https://badge.fury.io/rb/uringmachine.svg" alt="Ruby gem">
  </a>
  <a href="https://github.com/digital-fabric/uringmachine/actions">
    <img src="https://github.com/digital-fabric/uringmachine/actions/workflows/test.yml/badge.svg" alt="Tests">
  </a>
  <a href="https://github.com/digital-fabric/uringmachine/blob/master/LICENSE">
    <img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="MIT License">
  </a>
</p>

<p align="center">
  <a href="https://www.rubydoc.info/gems/uringmachine">API reference</a>
</p>

## What is UringMachine?

UringMachine is a Ruby gem for building fiber-based concurrent Ruby apps running
on Linux and using io_uring for performing I/O. UringMachine provides a
low-level API for performing concurrent I/O, as well as a full-featured
[`Fiber::Scheduler`](https://docs.ruby-lang.org/en/master/Fiber/Scheduler.html)
implementation that allows integration with the entire Ruby ecosystem.

## Features

- Automatic fiber switching when performing blocking I/O operations.
- Automatic cancellation using of ongoing operations with Ruby exceptions.
- General-purpose API for cancelling any operation on timeout.
- Excellent performance characteristics for concurrent I/O-bound applications.
- `Fiber::Scheduler` implementation to automatically integrate with the Ruby
  ecosystem in a transparent fashion.

## Design

In UringMachine, an I/O operation is performed by submitting it to the io_uring
interface (using the io_uring submission queue, or SQ) and waiting for a
corresponding entry to be added to the completion queue, or CQ. Since
UringMachine implements fiber-based concurrency, the fiber that performs the I/O
yields control after submitting the operation, and when a completion is
received, the fiber is scheduled to be resumed by putting it on the so-called
"runqueue", which is a queue of fibers ready to be resumed. When the runqueue is
exhausted, UringMachine enters the kernel in order to wait for one or more I/O
operation completions.

As a general rule, a single UringMachine instance is used for per thread,
managing the switching between the different fibers created on that same thread.
In addition, a UringMachine-based fiber scheduler may be installed in order to
allow any library that performs I/O using standard-library classes such as `IO`,
`TCPSocket` or `OpenSSL::SSL::SSLSocket`, and higher-level abstractions such as
`Net::HTTP` to perform I/O using UringMachine.

## Getting Started

To install UringMachine, simply run `gem install uringmachine` or `bundle add
uringmachine` in your project directory. Note: to use UringMachine, you'll need
a Linux machine with a minimum kernel version of 6.7. Some features require
newer kernel versions.

To perform I/O using UringMachine, simply create an instance:

```ruby
machine = UringMachine.new

# or alternatively
machine = UM.new
```

You can perform I/O by directly making method calls such as `write` or `read`
(for the full API see the reference.):

```ruby
# Most UringMachine instance methods will need you to provide a file descriptor.
# Here we print a message to STDOUT. Note the explicit line break:
machine.write(STDOUT, "Hello, world!\n")
```

UringMachine provides an I/O interface that is to a large degree equivalent to
the Unix standard C interface:

```ruby
# Constants used for the different I/O APIs are available under the
# UringMachine, or UM namespace.
fd = machine.open('foo.txt', UM::O_RDONLY)
buf = +''
size = machine.read(fd, buf, 8192)
machine.write(STDOUT, "File content: #{buf.inspect}")
machine.close(fd)

# Or alternatively (with automatic file closing):
machine.open('foo.txt', UM::O_RDONLY) do |fd|
  buf = +''
  size = machine.read(fd, buf, 8192)
  machine.write(STDOUT, "File content: #{buf.inspect}")
end
```

## Fiber control

To perform I/O operations concurrently, you can spin up new fibers by calling
`#spin`. You can wait for a fiber to terminate by calling `#join`:

```ruby
# This creates a pipe and returns the file descriptors for its read and write
# ends.
r_fd, w_fd = UM.pipe

# read from pipe
read_fiber = machine.spin do
  buf = +''
  loop do
    len = machine.read(r_fd, buf, 8192)
    break if len == 0

    # print to STDOUT
    machine.write(UM::STDOUT_FILENO, "#{buf}\n")
  end
end

write_fiber = machine.spin do
  (1..10).each do |count|
    machine.sleep(1)
    machine.write(w_fd, "#{count} Mississipi")
  end
  machine.close(w_fd)
end

# Wait for both fibers to finish running
machine.join(read_fiber, write_fiber)
```

You can also terminate a fiber by scheduling it manually. Normally this would be
done using an exception, which would cause the fiber to cancel whatever
operation it is currently waiting for, and run any `rescue` or `ensure` block:

```ruby
sleep_fiber = machine.spin do
  puts "Going to sleep..."
  machine.sleep(3)
  puts "Done sleeping."
rescue => e
  puts "Got error: #{e.inspect}"
end

# Let sleep_fiber start running
machine.sleep(0.1)
machine.schedule(sleep_fiber, RuntimeError.new('Cancel!'))
machine.join(sleep_fiber)
```

## Synchronization primitives

UringMachine also includes a io_uring-based implementation of a queue and a
mutex. `UM::Mutex` can be used for synchronizing access to a protected resource,
for example a database connection:

```ruby
class DBConnection
  def initialize(machine, db)
    @machine = machine
    @db = db
    @mutex = UM::Mutex.new
  end

def query(sql)
  @machine.synchronize(@mutex) do
    @db.query(sql)
  end
end
```

A queue can be used to coordinate between multiple fibers, for example a fiber
that consumes data and a fiber that produces data:

```ruby
queue = UM::Queue.new

producer = machine.spin do
  10.times do
    # lazy producer wants to sleep a bit
    machine.sleep(rand(0.1..1.5))
    machine.push(queue, rand(1000))
  end
  machine.push(queue, :STOP)
end

consumer = machine.spin do
  sum = 0
  loop do
    # the call to #shift blocks if the queue is empty
    value = machine.shift(queue)
    break if value == :STOP

    sum += value
  end
  puts "Sum: #{sum}"
end

machine.join(producer, consumer)
```

Note: to use the regular Ruby `Mutex` and `Queue` together with UringMachine,
you'll need to set up a fiber scheduler (see below).

## Sleeping and Working with Timeouts

The `#sleep` method allows a fiber to sleep for a period of time:

```ruby
puts "Sleeping"
machine.sleep(1)
puts "Done sleeping"
```

You can also perform operations periodically by calling `#periodically`:

```ruby
machine.spin do
  # This runs an infinite loop, invoking the block every 200 msecs
  machine.periodically(0.2) do
    machine.write(fd, "Hello")
  end
end
```

UringMachine also provides a uniform API for implementing timeouts. To add a
timeout to an operation, you need to wrap it with a call to `#timeout`:

```ruby
class TOError < StandardError; end;

# timeout after 3 seconds
machine.timeout(3, TOError) do
  # wait to shift an item from the queue
  value = machine.shift(queue)
rescue TOError
  value = nil
end
```

## The Fiber Scheduler

In order to allow UringMachine to integrate with the rest of the Ruby ecosystem,
and act as the underlying I/O layer, UringMachine includes a full-featured
implementation of the `Fiber::Scheduler` interface.

Note: in order to benefit a the fiber scheduler you'll need to create so-called
"non-blocking" fibers. This is usually achieved automatically in app servers
such as [Falcon](https://github.com/socketry/falcon/).

To start a fiber scheduler, you need to create an instance of
`UringMachine::FiberScheduler` for each thread where you'll be doing fiber-based
concurrent I/O:

```ruby
machine = UM.new
scheduler = UM::FiberScheduler.new(machine)
Fiber.set_scheduler(scheduler)
```

Once the scheduler in place, it's going to handle any I/O or other blocking
operation (such as sleeping, or locking a mutex, or waiting for a thread to
terminate) by offloading the operations to the underlying UringMachine instance,
*provided the I/O is performed inside of a non-blocking fiber. A non-blocking
fiber can be started either using `UringMachine#spin` or alternatively,
`Fiber.schedule`:

```ruby
machine = UM.new
scheduler = UM::FiberScheduler.new(machine)
Fiber.set_scheduler(scheduler)

fiber = Fiber.schedule do
  # this will sleep using underlying UringMachine instance. It is equivalent to
  # calling machine.sleep(0.05)
  sleep(0.05)
end
```

## Performance

[Detailed benchmarks](benchmark/README.md)

## API Reference

[API Reference](https://www.rubydoc.info/gems/uringmachine)
