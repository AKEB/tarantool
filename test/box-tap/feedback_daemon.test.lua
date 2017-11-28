#!/usr/bin/env tarantool

-- Testing feedback module

local tap = require('tap')
local json = require('json')
local fiber = require('fiber')
local test = tap.test('feedback_daemon')

test:plan(9)

box.cfg{log = 'report.log', log_level = 6}

local function self_decorator(self)
    return function(handler)
        return function(req) return handler(self, req) end
    end
end

-- set up mock for feedback server
local function get_feedback(self, req)
    local body = req:read()
    local ok, data = pcall(json.decode, body)
    if ok then
        self:put({ 'feedback', body })
    end
end

local interval = 0.01

box.cfg{
    feedback_host = '0.0.0.0:4444/feedback',
    feedback_interval = interval,
}

-- check it does not fail without server
local daemon = box.internal.feedback_daemon
daemon.start()
daemon.send_test()
local httpd = require('http.server').new('0.0.0.0', '4444')
httpd:route(
    { path = '/feedback', method = 'POST' },
    self_decorator(box.space._schema)(get_feedback)
)
httpd:start()

local function check(message)
    while box.space._schema:get('feedback') == nil do fiber.sleep(0.001) end
    local data = box.space._schema:get('feedback')
    test:ok(data ~= nil, message)
    box.space._schema:delete('feedback')
end

-- check if feedback has been sent and received
daemon.reload()
check("feedback received after reload")

local errinj = box.error.injection
errinj.set("ERRINJ_HTTPC", true)
check('feedback received after errinj')
errinj.set("ERRINJ_HTTPC", false)

daemon.send_test()
check("feedback received after explicit sending")

box.cfg{feedback_enabled = false}
daemon.send_test()
fiber.sleep(2 * interval)
test:ok(box.space._schema:get('feedback') == nil, "no feedback after disabling")

box.cfg{feedback_enabled = true}
daemon.send_test()
check("feedback after start")

daemon.stop()
daemon.send_test()
fiber.sleep(2 * interval)
test:ok(box.space._schema:get('feedback') == nil, "no feedback after stop")

daemon.start()
daemon.send_test()
check("feedback after start")

box.feedback.save("feedback.json")
daemon.send_test()
while box.space._schema:get('feedback') == nil do fiber.sleep(0.001) end
local data = box.space._schema:get('feedback')
local fio = require("fio")
local fh = fio.open("feedback.json")
test:ok(fh, "file is created")
local file_data = fh:read()
test:is(file_data, data[2], "data is equal")
fh:close()
fio.unlink("feedback.json")

test:check()
os.exit(0)
