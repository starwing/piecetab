-- piecetab Lua binding tests. run: just lua-test (cwd = repo root)
local dir = arg[0]:match("^(.*)[/\\]") or "."
package.path = dir .. "/?.lua;" .. package.path
package.cpath = (jit and "build/luajit/?.so;" or "build/lua55/?.so;")
                    .. package.cpath

local lu = require "luaunit"
local pt = require "piecetab"

TestModule = {}

function TestModule:testVersion()
    lu.assertIsString(pt.version)
end

-- Buffer tests

TestBuffer = {}

function TestBuffer:testFrom()
    local b = pt.from("hello\nworld")
    lu.assertEquals(#b, 11)
    lu.assertEquals(b:read(0), "hello\nworld")
end

function TestBuffer:testEmpty()
    local b = pt.empty()
    lu.assertEquals(#b, 0)
    lu.assertEquals(b:read(0), "")
end

function TestBuffer:testReadRange()
    local b = pt.from("hello\nworld")
    lu.assertEquals(b:read(6, 5), "world")
    lu.assertEquals(b:read(6), "world")
end

function TestBuffer:testReadClamp()
    local b = pt.from("hello\nworld")
    lu.assertEquals(b:read(100, 5), "")
    lu.assertEquals(b:read(6, 999), "world")
end

function TestBuffer:testPieces()
    local b = pt.from("hello\nworld")
    local parts = {}
    for off, len, s in b:pieces() do
        table.insert(parts, s)
        if #parts == 1 then
            lu.assertEquals(off, 0)
        end
    end
    local result = table.concat(parts)
    lu.assertEquals(result, "hello\nworld")
end

function TestBuffer:testCompact()
    local b = pt.from("hello\nworld")
    local c = b:compact()
    lu.assertEquals(c:read(0), "hello\nworld")
end

function TestBuffer:testDump()
    local b = pt.from("hello\nworld")
    lu.assertEquals(b:dump(), "hello\nworld")
end

function TestBuffer:testDumpEmpty()
    local b = pt.empty()
    lu.assertEquals(b:dump(), "")
end

-- Cursor tests

TestCursor = {}

function TestCursor:testOffset()
    local b = pt.from("hello\nworld")
    lu.assertEquals(b:cursor(3):offset(), 3)
    lu.assertEquals(b:cursor(999):offset(), 11)
end

function TestCursor:testAdvance()
    local b = pt.from("hello\nworld")
    local c = b:cursor(5)
    c:advance(-999)
    lu.assertEquals(c:offset(), 0)
end

function TestCursor:testReadAdvances()
    local b = pt.from("hello\nworld")
    local c = b:cursor(0)
    lu.assertEquals(c:read(5), "hello")
    lu.assertEquals(c:read(5), "\nworl")
end

function TestCursor:testAppendShort()
    local b = pt.empty()
    local c = b:cursor(0)
    c:append("abc")
    local b2 = c:commit()
    lu.assertEquals(b2:read(0), "abc")
end

function TestCursor:testAppendLong()
    local s = string.rep("x", 200)
    local b = pt.empty()
    local c = b:cursor(0)
    c:append(s)
    local b2 = c:commit()
    lu.assertEquals(#b2, 200)
    lu.assertEquals(b2:read(0), s)
end

function TestCursor:testInsert()
    local b = pt.from("helloworld")
    local c = b:cursor(5)
    c:insert(", ")
    local b2 = c:commit()
    lu.assertEquals(b2:read(0), "hello, world")
end

function TestCursor:testInsertOffset()
    local b = pt.from("helloworld")
    local c = b:cursor(5)
    c:insert(", ")
    lu.assertEquals(c:offset(), 5)
end

function TestCursor:testEditRemove()
    local b = pt.from("hello world")
    local c = b:cursor(0)
    c:edit(5, "X")
    local b2 = c:commit()
    lu.assertEquals(b2:read(0), "X world")
    local c2 = pt.from("hello world"):cursor(0)
    c2:edit(5, "X")
    c2:remove(5)
    local b3 = c2:commit()
    lu.assertEquals(b3:read(0), "Xd")
end

function TestCursor:testCowIsolation()
    local b = pt.from("original")
    local c = b:cursor(0)
    c:edit(8, "changed")
    local b2 = c:commit()
    lu.assertEquals(b:read(0), "original")
    lu.assertEquals(b2:read(0), "changed")
end

function TestCursor:testDetach()
    local b = pt.from("hello")
    local c = b:cursor(0)
    c:commit()
    lu.assertErrorMsgContains("invalid Cursor", function() c:read(1) end)
end

function TestCursor:testRollback()
    local b = pt.from("original")
    local c = b:cursor(0)
    c:edit(8, "changed")
    local b2 = c:rollback()
    lu.assertEquals(b2:read(0), "original")
end

function TestCursor:testChain()
    local b = pt.empty()
    local c = b:cursor(0)
    c:append("a"):append("b")
    local b2 = c:commit()
    lu.assertEquals(b2:read(0), "ab")
end

function TestCursor:testGC()
    local c
    do
        local b = pt.from("hello")
        c = b:cursor(0)
    end
    collectgarbage()
    collectgarbage()
    lu.assertEquals(c:read(3), "hel")
end

-- new tests

function TestCursor:testEditTooLong()
    local b = pt.from("hello world")
    local c = b:cursor(0)
    lu.assertErrorMsgContains("bad argument", function()
        c:edit(0, string.rep("x", pt.MAX_HOLESIZE + 1))
    end)
end

function TestCursor:testSplice()
    local b = pt.from("hello world")
    local c = b:cursor(0)
    c:splice(5, ", big")  -- delete "hello", insert ", big"
    local b2 = c:commit()
    lu.assertEquals(b2:read(0), ", big world")
end

function TestCursor:testSpliceLong()
    local long = string.rep("ab", 100)
    local b = pt.from("hix")
    local c = b:cursor(2)
    c:splice(0, long)  -- insert long at position (no delete)
    local b2 = c:commit()
    lu.assertEquals(b2:read(0), "hi" .. long .. "x")
    lu.assertEquals(#b2, #("hi" .. long .. "x"))
end

function TestCursor:testSpliceRemove()
    local b = pt.from("hello world")
    local c = b:cursor(0)
    c:splice(6, "")  -- delete "hello " (equiv to remove(6))
    local b2 = c:commit()
    lu.assertEquals(b2:read(0), "world")
end

function TestCursor:testReadBig()
    local big = string.rep("ab", 8192)
    local b = pt.from(big)
    -- full read
    lu.assertEquals(b:read(0), big)
    lu.assertEquals(#b, #big)
    -- range read
    local sub = b:read(8000, 200)
    lu.assertEquals(#sub, 200)
    lu.assertEquals(sub, string.sub(big, 8001, 8200))
end

function TestModule:testHolesizeConst()
    lu.assertEquals(pt.MAX_HOLESIZE, 64)
end

-- Doc tests

TestDoc = {}

function TestDoc:testNewEmpty()
    local d = pt.doc()
    lu.assertEquals(#d, 0)
    lu.assertEquals(d:read("a"), "")
end

function TestDoc:testNewFromString()
    local d = pt.doc("hello\nworld")
    lu.assertEquals(#d, 11)
    lu.assertEquals(d:read("a"), "hello\nworld")
end

function TestDoc:testNewFromBuffer()
    local b = pt.from("hi there")
    local d = pt.doc(b)
    lu.assertEquals(#d, 8)
    lu.assertEquals(d:read("a"), "hi there")
end

function TestDoc:testLen()
    local d = pt.doc("hello")
    lu.assertEquals(#d, 5)
end

function TestDoc:testSeekSet()
    local d = pt.doc("hello world")
    d:seek("set", 6)
    lu.assertEquals(d:seek(), 6)
    lu.assertEquals(d:read(5), "world")
end

function TestDoc:testSeekCur()
    local d = pt.doc("hello world")
    lu.assertEquals(d:seek("cur", 6), 6)
    lu.assertEquals(d:seek("cur", -3), 3)
end

function TestDoc:testSeekEnd()
    local d = pt.doc("hello")
    lu.assertEquals(d:seek("end"), 5)
    lu.assertEquals(d:seek("end", -3), 2)
end

function TestDoc:testOffset()
    local d = pt.doc("hello world")
    lu.assertEquals(d:offset(), 0)
    d:seek("set", 6)
    lu.assertEquals(d:offset(), 6)
    d:remove(3)
    lu.assertEquals(d:offset(), 6)
end

function TestDoc:testColumn()
    local d = pt.doc("hello\nworld")
    lu.assertEquals(d:column(), 0)
    d:seek("set", 2)
    lu.assertEquals(d:column(), 2)
    d:seek("set", 6)
    lu.assertEquals(d:column(), 0)  -- start of line 2
    d:seek("set", 9)
    lu.assertEquals(d:column(), 3)  -- "rld"
end

function TestDoc:testColumnAfterEdit()
    local d = pt.doc("hello world")
    d:seek("set", 5)
    lu.assertEquals(d:column(), 5)
    d:insert(",\n")
    -- insert doesn't advance; cursor stays at comma (col 5 of "hello,")
    lu.assertEquals(d:column(), 5)
    d:seek("set", 7)  -- after \n, start of " world"
    lu.assertEquals(d:column(), 0)
end

function TestDoc:testOffsetAfterWrite()
    local d = pt.doc("heo")
    d:seek("set", 2)
    d:write("ll")
    lu.assertEquals(d:offset(), 4)  -- write advances
end

function TestDoc:testReadN()
    local d = pt.doc("hello world")
    lu.assertEquals(d:read(5), "hello")
    lu.assertEquals(d:read(6), " world")
end

function TestDoc:testReadNEof()
    local d = pt.doc("hi")
    d:seek("end")
    lu.assertNil(d:read(1))
end

function TestDoc:testReadNZero()
    local d = pt.doc("hi")
    lu.assertEquals(d:read(0), "")
    d:seek("end")
    lu.assertEquals(d:read(0), "")
end

function TestDoc:testReadLine()
    local d = pt.doc("hello\nworld")
    lu.assertEquals(d:read("l"), "hello")
    lu.assertEquals(d:read("l"), "world")
    lu.assertNil(d:read("l"))
end

function TestDoc:testReadLineWithNL()
    local d = pt.doc("hello\nworld\n")
    lu.assertEquals(d:read("L"), "hello\n")
    lu.assertEquals(d:read("L"), "world\n")
    lu.assertNil(d:read("L"))
end

function TestDoc:testReadLineStar()
    local d = pt.doc("hello\nworld")
    lu.assertEquals(d:read("*l"), "hello")
end

function TestDoc:testReadAll()
    local d = pt.doc("hello\nworld")
    lu.assertEquals(d:read("a"), "hello\nworld")
    d:seek("set", 3)
    lu.assertEquals(d:read("a"), "lo\nworld")
    d:seek("end")
    lu.assertEquals(d:read("a"), "")
end

function TestDoc:testWrite()
    local d = pt.doc("heo")
    d:seek("set", 2)
    d:write("ll")
    -- write advances cursor past written text, rewind to read
    d:seek("set", 0)
    lu.assertEquals(d:read("a"), "hello")
end

function TestDoc:testInsert()
    local d = pt.doc("hllo")
    d:seek("set", 1)
    d:insert("e")
    lu.assertEquals(d:seek(), 1)  -- insert doesn't advance
    d:seek("set", 0)
    lu.assertEquals(d:read("a"), "hello")
end

function TestDoc:testAppend()
    local d = pt.doc("hello")
    d:append(" world")
    -- append positions at end, rewind to read
    d:seek("set", 0)
    lu.assertEquals(d:read("a"), "hello world")
end

function TestDoc:testSplice()
    local d = pt.doc("hello world")
    d:seek("set", 5)
    d:splice(6, " everybody")
    d:seek("set", 0)
    lu.assertEquals(d:read("a"), "hello everybody")
end

function TestDoc:testRemove()
    local d = pt.doc("hello world")
    d:seek("set", 5)
    d:remove(6)
    d:seek("set", 0)
    lu.assertEquals(d:read("a"), "hello")
end

function TestDoc:testLine()
    local d = pt.doc("a\nb\nc")
    lu.assertEquals(d:line(), 0)
    d:seek("set", 2)
    lu.assertEquals(d:line(), 1)
    d:seek("set", 4)
    lu.assertEquals(d:line(), 2)
end

function TestDoc:testLineLen()
    -- lc stores line lengths including \n for terminated lines
    local d = pt.doc("hello\nworld\n!")
    lu.assertEquals(d:linelen(0), 6)  -- "hello\n" = 6
    lu.assertEquals(d:linelen(1), 6)  -- "world\n" = 6
    lu.assertEquals(d:linelen(2), 1)  -- "!" = 1
end

function TestDoc:testLineLenCurrent()
    local d = pt.doc("hello\nlonger\n")
    d:seek("set", 0)
    lu.assertEquals(d:linelen(), 6)  -- "hello\n"
    d:seek("set", 6)
    lu.assertEquals(d:linelen(), 7)  -- "longer\n"
end

function TestDoc:testCommit()
    local d = pt.doc("hello")
    local v1 = d:commit()
    lu.assertIsNumber(v1)
    lu.assertEquals(v1, 1)
    d:seek("end")
    d:write(" world")
    local v2 = d:commit()
    lu.assertEquals(v2, 2)
    d:seek("set", 0)
    lu.assertEquals(d:read("a"), "hello world")
end

function TestDoc:testCommitNoop()
    local d = pt.doc("hi")
    local v1 = d:commit()
    lu.assertEquals(v1, 1)
    local v2 = d:commit()  -- no edits
    lu.assertEquals(v2, 1)  -- still at vid 1
end

function TestDoc:testUndoRedo()
    local d = pt.doc("")
    d:seek("end"); d:write("hello"); d:commit()
    d:seek("end"); d:write(" world"); d:commit()
    -- now "hello world"
    d:seek("set", 0)
    lu.assertEquals(d:read("a"), "hello world")
    -- undo
    local vid = d:undo()
    lu.assertEquals(vid, 2)
    -- after undo, cursor at 0, reads full "hello"
    lu.assertEquals(d:read("a"), "hello")
    -- redo
    d:seek("set", 0)
    vid = d:redo()
    lu.assertEquals(vid, 3)
    d:seek("set", 0)
    lu.assertEquals(d:read("a"), "hello world")
end

function TestDoc:testUndoMultiple()
    local d = pt.doc("")
    d:seek("end"); d:write("a"); d:commit()
    d:seek("end"); d:write("b"); d:commit()
    d:seek("end"); d:write("c"); d:commit()
    -- "abc"
    d:undo(); d:undo()
    -- cursor at 0, reads "a"
    lu.assertEquals(d:read("a"), "a")
    d:redo()
    d:seek("set", 0)
    lu.assertEquals(d:read("a"), "ab")
end

function TestDoc:testUndoBranch()
    local d = pt.doc("")
    d:seek("end"); d:write("hello"); d:commit()
    d:seek("end"); d:write(" world"); d:commit()  -- "hello world"
    d:undo()  -- back to "hello"
    d:seek("end"); d:write(" there"); d:commit()  -- "hello there" (branch)
    d:seek("set", 0)
    lu.assertEquals(d:read("a"), "hello there")
    -- undo twice: back to root, then forward to child[0]
    d:undo()  -- back to "hello"
    d:redo()  -- redo takes first child = "hello world"
    d:seek("set", 0)
    lu.assertEquals(d:read("a"), "hello world")
    -- confirm undo(vid) can jump directly to any version
    d:undo(); d:undo()  -- back to root (vid 1, "")
    d:undo(4)  -- jump to vid 4 ("hello there")
    d:seek("set", 0)
    lu.assertEquals(d:read("a"), "hello there")
end

function TestDoc:testBufferExport()
    local d = pt.doc("hello")
    d:seek("end")
    d:write(" world")
    d:commit()
    local b = d:buffer()
    lu.assertEquals(b:read(0), "hello world")
end

function TestDoc:testBufferExportVid()
    local d = pt.doc("hello")
    d:seek("end")
    d:write(" world")
    d:commit()
    d:seek("end")
    d:write(" and more")
    d:commit()
    local b = d:buffer(2)
    lu.assertEquals(b:read(0), "hello world")
    local b2 = d:buffer(3)
    lu.assertEquals(b2:read(0), "hello world and more")
end

function TestDoc:testDump()
    local d = pt.doc("hello\nworld")
    lu.assertEquals(d:dump(), "hello\nworld")
end

function TestDoc:testDumpEmpty()
    local d = pt.doc()
    lu.assertEquals(d:dump(), "")
end

function TestDoc:testDumpPreservesCursor()
    local d = pt.doc("hello world")
    d:seek("set", 6)
    local s = d:dump()
    lu.assertEquals(s, "hello world")
    lu.assertEquals(d:offset(), 6)  -- cursor unchanged
end

function TestDoc:testWriteChain()
    local d = pt.doc("")
    d:seek("end"); d:write("a"):write("b")
    -- chained writes: each writes at cursor, cursor advances
    d:seek("set", 0)
    lu.assertEquals(d:read("a"), "ab")
end

function TestDoc:testMultipleEdits()
    local d = pt.doc("")
    d:write("x"):commit()
    d:seek("end")
    d:write("y"):commit()
    d:undo()
    d:seek("end")
    d:write("z"):commit()
    d:seek("set", 0)
    lu.assertEquals(d:read("a"), "xz")
end

function TestDoc:testReadLineEmptyLastLine()
    local d = pt.doc("line1\n")
    lu.assertEquals(d:read("l"), "line1")
    d:read("l")  -- last empty line
    lu.assertNil(d:read("l"))
end

function TestDoc:testReadLineEOF()
    local d = pt.doc("")
    lu.assertNil(d:read("l"))
end

function TestDoc:testFreshUndoDiscards()
    local d = pt.doc("abc\ndef\nzzz")
    d:seek("line", 2)
    d:insert("xxx\nyyy\n")
    lu.assertEquals(d:dump(), "abc\ndef\nxxx\nyyy\nzzz")
    d:undo()
    lu.assertEquals(d:dump(), "abc\ndef\nzzz")
end

function TestDoc:testFreshUndoCannotRedo()
    local d = pt.doc("hello")
    d:write(" world")
    d:undo()
    lu.assertEquals(d:dump(), "hello")
    -- after discarding fresh edits, redo has nowhere to go
    lu.assertEquals(d:redo(), d:commit())
    lu.assertEquals(d:dump(), "hello")
end

function TestDoc:testLineCount()
    local d = pt.doc("a\nb\nc")
    lu.assertEquals(d:linecount(), 3)
    d:seek("end"); d:write("\nd")
    lu.assertEquals(d:linecount(), 4)
end

function TestDoc:testLineCountEmpty()
    local d = pt.doc("")
    lu.assertEquals(d:linecount(), 1)
end

function TestDoc:testLines()
    local d = pt.doc("a\nb\nc")
    local lines = {}
    for lnum, text in d:lines() do
        table.insert(lines, {lnum, text})
    end
    lu.assertEquals(#lines, 3)
    lu.assertEquals(lines[1][1], 0); lu.assertEquals(lines[1][2], "a")
    lu.assertEquals(lines[2][1], 1); lu.assertEquals(lines[2][2], "b")
    lu.assertEquals(lines[3][1], 2); lu.assertEquals(lines[3][2], "c")
end

function TestDoc:testLinesEmpty()
    local d = pt.doc("")
    local count = 0
    for _ in d:lines() do count = count + 1 end
    lu.assertEquals(count, 1)
end

function TestDoc:testLinesTrailingNL()
    local d = pt.doc("x\n")
    local lines = {}
    for lnum, text in d:lines() do
        table.insert(lines, text)
    end
    lu.assertEquals(#lines, 1)
    lu.assertEquals(lines[1], "x")
end

os.exit(lu.LuaUnit.run())
