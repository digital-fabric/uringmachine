# frozen_string_literal: true

require 'bundler/inline'

gemfile do
  source 'https://rubygems.org'
  gem 'benchmark'
  gem 'benchmark-ips'
end

require 'benchmark/ips'

r, w = IO.pipe

class IO
  def __fd__
    @__fd__ ||= fileno
  end
end

@map = ObjectSpace::WeakMap.new

def cached_fileno(r)
  @map[r] ||= r.fileno
end

Benchmark.ips do |x|
  x.report('IO#fileno')       { r.fileno }
  x.report('cached_fileno')   { cached_fileno(r) }
  x.report('__fd__')          { r.__fd__ }

  x.compare!(order: :baseline)
end
