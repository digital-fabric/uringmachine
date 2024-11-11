# UringMachine

<a href="http://rubygems.org/gems/uringmachine">
  <img src="https://badge.fury.io/rb/uringmachine.svg" alt="Ruby gem">
</a>
<a href="https://github.com/digital-fabric/uringmachine/actions?query=workflow%3ATests">
  <img src="https://github.com/digital-fabric/uringmachine/workflows/Tests/badge.svg" alt="Tests">
</a>
<a href="https://github.com/digital-fabric/uringmachine/blob/master/LICENSE">
  <img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="MIT License">
</a>

UringMachine is a fiber-based library for creating concurrent apps in Ruby on
modern Linux machines. UringMachine provides a rich API for performing I/O using
[io_uring](https://en.wikipedia.org/wiki/Io_uring).

## Features

- Automatic fiber switching when performing blocking operations.
- Automatic cancellation using of ongoing operations with Ruby exceptions.
- General-purpose API for cancelling any operation on timeout.
- High performance (needs to be proved).
- (Eventually) I/O class with buffered reads and an intuitive API.

## Design

UringMachine is based on my experience marrying Ruby and io_uring:

- [Polyphony](https://github.com/digital-fabric/polyphony) - a comprehensive gem
  providing io_uring functionality, structured concurrency, and monkey-patching
  for the Ruby standard library.
- [IOU](https://github.com/digital-fabric/iou) - a low-level asynchronous API
  for using io_uring from Ruby.

### Learnings

Some important learnings from those two projects, in no particular order:

- Monkey-patching is not a good solution, long term. You need to deal with
  changing APIs (Ruby is evolving quite rapidly these days!), and anyways you're
  always going to get stuck with some standard Ruby API that's implemented as a
  C extension and just won't play nice with whatever you're trying to do.
- The design of the Polyphony io_uring backend was an evolution of something
  that was originally based on libev as an event loop. In hindsight, adapting
  the design for how io_uring worked led to code that was too complex and even
  somewhat brittle.
- IOU showed me that even if we embrace callbacks, the developer experience is
  substantially inferior to what you can do with a sequential coding style. Even
  just in terms of line count - with callbacks you end up with roughly double
  the number of lines of code.
- Implementing fiber switching on top of IOU was disappointing in terms of
  performance. In order for a fiber-based solution to be performed it had to be
  baked in - hence UringMachine.
- Working with fibers has the very important benefit that you can keep stuff on
  the stack, instead of passing around all kinds of references to the heap. In
  addition, you mostly don't need to worry about marking Ruby objects used in
  operations, since normally they'll already be on the stack as method call
  parameters.
- Polyphony was designed as an all-in-one solution that did everything: turning
  stock APIs into fiber-aware ones, providing a solid structured-concurrency
  implementation for controlling fiber life times, extensions providing
  additional features such as compressing streaming data between two fds, other
  APIs based on splicing etc. Perhaps a more cautious approach would be better.
- Pending operation lifetime management in Polyphony was based a complex
  reference counting scheme that proved problematic, especially for multishot
  operations.

So, based on those two projects, I wanted to design a Ruby API for io_uring
based on the following principles:

- Automatic fiber switching.
- No monkey-patching. Instead, provide a simple custom API, as a replacement for
  the stock Ruby `IO` and `Socket` classes.
- Simpler management of pending operation lifetime.
- Do not insist on structured concurrency, just provide the APIs necessary to
  create actors and to supervise the execution of fibers.

### Cancellation

When working with io_uring, managing the life cycle of asynchronous operations
is quite tricky, especially with regards to cancellation. This is due to the
fact each operation lives on both sides of the userspace-kernel divide. This
means that when cancelling an operation, we cannot free, or dispose of any
resources associated with the operation, until we know for sure that the kernel
side is also done with the operation.

As stated above, working with fibers allows us to keep operation metadata and
associated data (such as buffers etc) on the stack, which can greatly simplify
the managing of the operation's lifetime, as well as significantly reduce heap
allocations.

When a cancellation does occur, UringMachine issues a cancellation (using
`io_uring_prep_cancel64`), and then waits for the corresponding CQE (with a
`-ECANCELED` result).

## Short Example

```ruby
require 'uringmachine'

machine = UringMachine.new
stdout_fd = STDOUT.fileno
stdin_fd = STDIN.fileno
machine.write(stdout_fd, "Hello, world!\n")

loop do
  machine.write(stdout_fd, "Say something: ")
  buf = +''
  res = machine.read(stdin_fd, buf, 8192)
  if res > 0
    machine.write(stdout_fd, "You said: #{buf}")
  else
    break
  end
end
```
