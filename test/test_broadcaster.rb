# frozen_string_literal: true

require_relative 'helper'
require 'uringmachine/broadcaster'

class BroadcasterTest < UMBaseTest
  def test_broadcaster_basic_usage
    buf = []
    broadcaster = UM::Broadcaster.new(machine)
    f1 = machine.spin do
      broadcaster.each { |v| buf << [:f1, v] }
    end
    f2 = machine.spin do
      broadcaster.each { |v| buf << [:f2, v] }
    end

    assert_equal 0, broadcaster.listener_count
    3.times { machine.snooze }
    assert_equal 2, broadcaster.listener_count    
    broadcaster.broadcast(:foo)
    3.times { machine.snooze }
    assert_equal [[:f1, :foo], [:f2, :foo]], buf

    broadcaster.broadcast(:bar)
    3.times { machine.snooze }
    assert_equal [[:f1, :foo], [:f2, :foo], [:f1, :bar], [:f2, :bar]], buf
  end

  def test_broadcaster_listener_break
    buf = []
    broadcaster = UM::Broadcaster.new(machine)
    f1 = machine.spin do
      broadcaster.each { |v| buf << [:f1, v]; break }
    end
    f2 = machine.spin do
      broadcaster.each { |v| buf << [:f2, v] }
    end

    3.times { machine.snooze }
    assert_equal 2, broadcaster.listener_count
    broadcaster.broadcast(:foo)
    3.times { machine.snooze }
    assert_equal [[:f1, :foo], [:f2, :foo]], buf

    assert_equal 1, broadcaster.listener_count
    broadcaster.broadcast(:bar)
    3.times { machine.snooze }
    assert_equal [[:f1, :foo], [:f2, :foo], [:f2, :bar]], buf
  end

  def test_broadcaster_listener_exception
    buf = []
    broadcaster = UM::Broadcaster.new(machine)
    f1 = machine.spin do
      broadcaster.each { |v| buf << [:f1, v]; raise }
    rescue => e
      buf << [:f1, :exception]
    end
    f2 = machine.spin do
      broadcaster.each { |v| buf << [:f2, v] }
    end

    3.times { machine.snooze }
    assert_equal 2, broadcaster.listener_count
    broadcaster.broadcast(:foo)
    3.times { machine.snooze }
    assert_equal [[:f1, :foo], [:f1, :exception], [:f2, :foo]], buf

    assert_equal 1, broadcaster.listener_count
    broadcaster.broadcast(:bar)
    3.times { machine.snooze }
    assert_equal [[:f1, :foo], [:f1, :exception], [:f2, :foo], [:f2, :bar]], buf
  end

  def test_broadcaster_helper_method
    buf = []
    broadcaster = machine.broadcaster
    assert_kind_of UM::Broadcaster, broadcaster
    f1 = machine.spin do
      broadcaster.each { |v| buf << [:f1, v] }
    end
    f2 = machine.spin do
      broadcaster.each { |v| buf << [:f2, v] }
    end

    assert_equal 0, broadcaster.listener_count
    3.times { machine.snooze }
    assert_equal 2, broadcaster.listener_count    
    broadcaster.broadcast(:foo)
    3.times { machine.snooze }
    assert_equal [[:f1, :foo], [:f2, :foo]], buf

    broadcaster.broadcast(:bar)
    3.times { machine.snooze }
    assert_equal [[:f1, :foo], [:f2, :foo], [:f1, :bar], [:f2, :bar]], buf
  end
end
