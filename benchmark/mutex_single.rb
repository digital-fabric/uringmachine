# frozen_string_literal: true

require 'bundler/inline'

gemfile do
  source 'https://rubygems.org'
  gem 'uringmachine', path: '..'
  gem 'benchmark'
  gem 'benchmark-ips'
end

require 'benchmark/ips'
require 'uringmachine'
require 'thread'

def threads_setup
  @threads_mutex = Mutex.new
end

def um_setup
  @um_machine = UM.new
  @um_mutex = UM::Mutex.new
end

threads_setup
um_setup

Benchmark.ips do |x|
  x.report("threads") { @threads_mutex.synchronize { } }
  x.report("UM")      { @um_machine.synchronize(@um_mutex) {} }

  x.compare!(order: :baseline)
end
