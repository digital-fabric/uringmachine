# frozen_string_literal: true

Dir["#{__dir__}/bm_*.rb"].each do |fn|
  puts "* #{File.basename(fn)}"
  puts
  puts `ruby -W0 --yjit #{fn}`
  puts
end
