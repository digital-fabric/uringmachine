# frozen_string_literal: true

require_relative 'helper'
require 'socket'
require 'uringmachine/actor'

class ActorTest < UMBaseTest
  module Counter
    def setup
      @count = 0      
    end

    def incr
      @count += 1
    end

    def get
      @count
    end

    def reset
      @count = 0
    end
  end

  def test_basic_actor_functionality
    actor = @machine.spin_actor(Counter)
    
    assert_kind_of Fiber, actor

    assert_equal 0, actor.call(:get)
    assert_equal 1, actor.call(:incr)
    assert_equal actor, actor.cast(:incr)
    assert_equal 2, actor.call(:get)
    assert_equal actor, actor.cast(:reset)
    assert_equal 0, actor.call(:get)
  end

  module Counter2
    def setup(count)
      @count = count
    end

    def incr
      @count += 1
    end

    def get
      @count
    end

    def reset
      @count = 0
    end
  end


  def test_actor_with_args
    actor = @machine.spin_actor(Counter2, 43)
    
    assert_equal 43, actor.call(:get)
  end
end
