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

## Prior art

UringMachine is based on my experience marrying Ruby and io_uring:

- [Polyphony](https://github.com/digital-fabric/polyphony) - a comprehensive gem
  providing io_uring functionality, structured concurrency, and monkey-patching
  for the Ruby standard library.
- [IOU](https://github.com/digital-fabric/iou) - a low-level asynchronous API
  for using io_uring from Ruby.

## Example

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
