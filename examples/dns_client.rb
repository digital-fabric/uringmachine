# frozen_string_literal: true

require_relative '../lib/uringmachine'
require 'resolv'

machine = UM.new

addrs = machine.resolve('status.realiteq.net')

puts '*' * 40
puts addrs.join("\n")
puts
