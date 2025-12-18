# frozen_string_literal: true

require 'bundler/inline'

gemfile do
  source 'https://rubygems.org'
  gem 'io-event'
  gem 'async'
end


require 'async'
require 'securerandom'

selector ||= IO::Event::Selector::URing.new(Fiber.current)
scheduler = Async::Scheduler.new(selector:)
Fiber.set_scheduler scheduler

fn = "/tmp/file_io_#{SecureRandom.hex}"

scheduler.run do
  Fiber.schedule do
    File.open(fn, 'w') {
      it << 'foo'
      p pre_flush: IO.read(fn)
      it.flush
      it << 'bar'
      p post_flush: IO.read(fn)

    }
    p post_close: IO.read(fn)
  end
end
