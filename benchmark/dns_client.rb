# frozen_string_literal: true

require 'bundler/inline'

gemfile do
  source 'https://rubygems.org'
  gem 'uringmachine', path: '..'
  gem 'benchmark'
  gem 'benchmark-ips'
end

require 'uringmachine'
require 'resolv'

def do_addrinfo
  Addrinfo.tcp("status.realiteq.net", 80)
end

def do_resolv
  Resolv.getaddresses('status.realiteq.net')
end

@machine = UM.new
def do_um
  @machine.resolve('status.realiteq.net')
end

# p do_addrinfo
# p do_resolv
# p do_um
# exit

Benchmark.ips do |x|
  x.report("Addrinfo")    { do_addrinfo }
  x.report("resolv")      { do_resolv }
  x.report("UM.resolve")  { do_um }

  x.compare!(order: :baseline)
end



# addrs = machine.resolve('status.realiteq.net')

# puts '*' * 40
# puts addrs.join("\n")
# puts
