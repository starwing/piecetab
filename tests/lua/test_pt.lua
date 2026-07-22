-- piecetab Lua binding tests. run: just lua-test (cwd = repo root)
local dir = arg[0]:match("^(.*)[/\\]") or "."
package.path = dir .. "/?.lua;" .. package.path
package.cpath = (_G["jit"] and "build/luajit/?.so;" or "build/lua55/?.so;")
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

function TestBuffer:testFromEmpty()
    local b = pt.from("")
    lu.assertEquals(#b, 0)
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
    c:splice(5, ", big") -- delete "hello", insert ", big"
    local b2 = c:commit()
    lu.assertEquals(b2:read(0), ", big world")
end

function TestCursor:testSpliceLong()
    local long = string.rep("ab", 100)
    local b = pt.from("hix")
    local c = b:cursor(2)
    c:splice(0, long) -- insert long at position (no delete)
    local b2 = c:commit()
    lu.assertEquals(b2:read(0), "hi" .. long .. "x")
    lu.assertEquals(#b2, #("hi" .. long .. "x"))
end

function TestCursor:testSpliceRemove()
    local b = pt.from("hello world")
    local c = b:cursor(0)
    c:splice(6, "") -- delete "hello " (equiv to remove(6))
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

function TestCursor:testLocate()
    local b = pt.from("hello\nworld")
    local c = b:cursor(3)
    c:locate(6)
    lu.assertEquals(c:offset(), 6)
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

function TestDoc:testSeekEndPos()
    local d = pt.doc("hello world")
    d:seek("end", 3) -- positive offset from end = absolute position 3
    lu.assertEquals(d:offset(), 3)
end

function TestDoc:testSeekBadWhence()
    -- unknown whence falls through to return current position (no-op)
    local d = pt.doc("hi")
    d:seek("set", 1)
    ---@diagnostic disable-next-line: param-type-mismatch
    d:seek("xyz")
    lu.assertEquals(d:offset(), 1)
    -- string starting with 's' but not "se" falls through
    ---@diagnostic disable-next-line: param-type-mismatch
    d:seek("sx")
    lu.assertEquals(d:offset(), 1)
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
    lu.assertEquals(d:column(), 0) -- start of line 2
    d:seek("set", 9)
    lu.assertEquals(d:column(), 3) -- "rld"
end

function TestDoc:testColumnAfterEdit()
    local d = pt.doc("hello world")
    d:seek("set", 5)
    lu.assertEquals(d:column(), 5)
    d:insert(",\n")
    -- insert doesn't advance; cursor stays at comma (col 5 of "hello,")
    lu.assertEquals(d:column(), 5)
    d:seek("set", 7) -- after \n, start of " world"
    lu.assertEquals(d:column(), 0)
end

function TestDoc:testOffsetAfterWrite()
    local d = pt.doc("heo")
    d:seek("set", 2)
    d:write("ll")
    lu.assertEquals(d:offset(), 4) -- write advances
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
    lu.assertEquals(d:seek(), 1) -- insert doesn't advance
    d:seek("set", 0)
    lu.assertEquals(d:read("a"), "hello")
end

function TestDoc:testAppend()
    local d = pt.doc("hello")
    d:seek("end")
    d:append(" world")
    -- append positions at insertion end, rewind to read
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
    lu.assertEquals(d:linelen(0), 6) -- "hello\n" = 6
    lu.assertEquals(d:linelen(1), 6) -- "world\n" = 6
    lu.assertEquals(d:linelen(2), 1) -- "!" = 1
end

function TestDoc:testLineLenCurrent()
    local d = pt.doc("hello\nlonger\n")
    d:seek("set", 0)
    lu.assertEquals(d:linelen(), 6) -- "hello\n"
    d:seek("set", 6)
    lu.assertEquals(d:linelen(), 7) -- "longer\n"
end

function TestDoc:testCommit()
    local d = pt.doc("hello")
    local v1 = d:commit()
    lu.assertIsNumber(v1)
    d:seek("end")
    d:write(" world")
    local v2 = d:commit()
    lu.assertTrue(v2 > v1)
    d:seek("set", 0)
    lu.assertEquals(d:read("a"), "hello world")
end

function TestDoc:testCommitNoop()
    local d = pt.doc("hi")
    local v1 = d:commit()
    local v2 = d:commit() -- no edits
    lu.assertEquals(v2, v1)
end

function TestDoc:testUndoRedo()
    local d = pt.doc("")
    local v0 = d:commit()
    d:seek("end"); d:write("hello"); local v1 = d:commit()
    d:seek("end"); d:write(" world"); local v2 = d:commit()
    -- now "hello world"
    d:seek("set", 0)
    lu.assertEquals(d:read("a"), "hello world")
    -- undo to parent
    local vid = d:undo()
    lu.assertEquals(vid, v1)
    d:seek("set", 0)
    lu.assertEquals(d:read("a"), "hello")
    -- redo to first child
    d:seek("set", 0)
    vid = d:redo()
    lu.assertEquals(vid, v2)
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
    d:seek("set", 0)
    lu.assertEquals(d:read("a"), "a")
    d:redo()
    d:seek("set", 0)
    lu.assertEquals(d:read("a"), "ab")
end

function TestDoc:testUndoBranch()
    local d = pt.doc("")
    local v0 = d:commit()
    d:seek("end"); d:write("hello"); local v1 = d:commit()
    d:seek("end"); d:write(" world"); local v2 = d:commit() -- "hello world"
    d:undo()                                                -- back to "hello"
    d:seek("end"); d:write(" there"); local vb = d:commit() -- "hello there" (branch)
    d:seek("set", 0)
    lu.assertEquals(d:read("a"), "hello there")
    -- undo twice: back to root, then forward to child[0]
    d:undo() -- back to "hello"
    d:redo() -- redo takes first child = "hello world"
    d:seek("set", 0)
    lu.assertEquals(d:read("a"), "hello world")
    -- confirm undo(vid) can jump directly to any version
    d:undo(); d:undo() -- back to root (vid v0, "")
    d:undo(vb)         -- jump to branch version
    d:seek("set", 0)
    lu.assertEquals(d:read("a"), "hello there")
end

function TestDoc:testUndoTree()
    -- Build: root("") -> A("abc") -> B("abcXYZ") -> C("abcXYZ!")
    -- Then undo to B, branch: D("abcXYZ123") as child of B
    -- Tree: B has kids [C, D], ut_firstchild → C (oldest)
    local d = pt.doc("")
    d:commit()
    d:seek("end"); d:write("abc"); d:commit()            -- A "abc"
    d:seek("end"); d:write("XYZ"); local vB = d:commit() -- B "abcXYZ"
    d:seek("end"); d:write("!"); local vC = d:commit()   -- C "abcXYZ!"
    -- Undo to B, branch
    d:undo()                                             -- -> B, cursor at 6 (end of "abcXYZ")
    lu.assertEquals(d:offset(), 6)
    lu.assertEquals(d:dump(), "abcXYZ")
    d:write("123"); local vD = d:commit() -- D "abcXYZ123"
    d:seek("set", 0)
    lu.assertEquals(d:read("a"), "abcXYZ123")
    -- Jump C -> B -> redo(C)
    d:undo(vC) -- jump D -> C
    lu.assertEquals(d:dump(), "abcXYZ!")
    lu.assertEquals(d:offset(), 7)
    d:undo() -- C -> B
    lu.assertEquals(d:dump(), "abcXYZ")
    lu.assertEquals(d:offset(), 6)
    d:redo() -- B -> firstchild = C (oldest)
    lu.assertEquals(d:dump(), "abcXYZ!")
    lu.assertEquals(d:offset(), 6)
    -- undo(C) + redo → still C, cursor preserved
    d:undo(); d:redo()
    lu.assertEquals(d:dump(), "abcXYZ!")
    lu.assertEquals(d:offset(), 6)
    -- Jump back to D via vid
    d:undo(vD)
    lu.assertEquals(d:dump(), "abcXYZ123")
    lu.assertEquals(d:offset(), 6)
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
    local v2 = d:commit()
    d:seek("end")
    d:write(" and more")
    local v3 = d:commit()
    local b = d:buffer(v2)
    lu.assertEquals(b:read(0), "hello world")
    local b2 = d:buffer(v3)
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
    lu.assertEquals(d:offset(), 6) -- cursor unchanged
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
    d:read("l") -- last empty line
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

function TestDoc:testRedoFreshError()
    local d = pt.doc("hello")
    d:write(" world") -- uncommitted
    lu.assertErrorMsgContains("uncommitted",
        function() d:redo() end)
end

function TestDoc:testLineCount()
    local d = pt.doc("a\nb\nc")
    lu.assertEquals(d:breaks(), 3)
    d:seek("end"); d:write("\nd")
    lu.assertEquals(d:breaks(), 4)
end

function TestDoc:testLineCountEmpty()
    local d = pt.doc("")
    lu.assertEquals(d:breaks(), 0)
end

function TestDoc:testBreaksAfterAppend()
    -- append at pos 0 should not change break count
    local d = pt.doc("hello\nworld\n")
    lu.assertEquals(d:breaks(), 2)
    d:seek(0); d:append("X")
    lu.assertEquals(d:breaks(), 2)
end

function TestDoc:testBreaksAfterAppendMiddle()
    -- append in middle of first line
    local d = pt.doc("hello\nworld\n")
    lu.assertEquals(d:breaks(), 2)
    d:seek(2); d:append("X")
    lu.assertEquals(d:breaks(), 2)
end

function TestDoc:testBreaksAfterAppendPastNL()
    -- append after \n (shifts \n position, line count unchanged)
    local d = pt.doc("hello\nworld\n")
    lu.assertEquals(d:breaks(), 2)
    d:seek(6); d:append("X")  -- at "world"
    lu.assertEquals(d:breaks(), 2)
end

function TestDoc:testLinesCorruptsSeekLine()
    -- lines() iterates via read() which advances cursor, but
    -- must not cause re-application of fresh hunks in lc.
    local d = pt.doc("L0\nL1\nL2\nL3\nL4\nL5\nL6\n")
    d:append("X")
    d:breaks(); d:seek("line", 0)
    for _ in d:lines() do end
    d:seek("line", 4)
    lu.assertEquals(d:offset(), 13)
    lu.assertEquals(d:read("l"), "L4")
end

function TestDoc:testLines()
    -- lines() with no args = iterator that calls read() until nil
    -- (like io.lines: reads lines without \n)
    local d = pt.doc("a\nb\nc")
    local lines = {}
    for text in d:lines() do
        table.insert(lines, text)
    end
    lu.assertEquals(#lines, 3)
    lu.assertEquals(lines[1], "a")
    lu.assertEquals(lines[2], "b")
    lu.assertEquals(lines[3], "c")
end

function TestDoc:testLinesEmpty()
    -- empty doc: read() returns nil immediately, 0 iterations
    local d = pt.doc("")
    local count = 0
    for _ in d:lines() do count = count + 1 end
    lu.assertEquals(count, 0)
end

function TestDoc:testLinesTrailingNL()
    -- "x\n": lines() reads "x" then nil (trailing empty line not yielded)
    local d = pt.doc("x\n")
    local lines = {}
    for text in d:lines() do
        table.insert(lines, text)
    end
    lu.assertEquals(#lines, 1)
    lu.assertEquals(lines[1], "x")
end

function TestDoc:testBreaksLinesMismatchTrailingNL()
    -- File ending with \n: breaks() counts trailing empty line,
    -- but lines() does not yield it. This mismatch causes
    -- editor G command to leave a ghost row at screen bottom.
    local d = pt.doc("a\nb\n")
    lu.assertEquals(d:breaks(), 2)   -- should match lines() yield count

    local yielded = {}
    for text in d:lines() do
        table.insert(yielded, text)
    end
    lu.assertEquals(#yielded, 2)
    lu.assertEquals(yielded[1], "a")
    lu.assertEquals(yielded[2], "b")

    -- Without trailing \n: consistent (already passing)
    local d2 = pt.doc("a\nb")
    lu.assertEquals(d2:breaks(), 2)
    local c = 0
    for _ in d2:lines() do c = c + 1 end
    lu.assertEquals(c, 2)
end

function TestDoc:testLinesFormatL()
    -- lines("*l") = read("*l") each iteration
    local d = pt.doc("a\nb\n")
    local lines = {}
    for text in d:lines("*l") do
        table.insert(lines, text)
    end
    lu.assertEquals(#lines, 2)
    lu.assertEquals(lines[1], "a")
    lu.assertEquals(lines[2], "b")
end

function TestDoc:testLinesFormatNL()
    -- lines("*L") = read("*L") each iteration (lines with \n)
    local d = pt.doc("a\nb\n")
    local lines = {}
    for text in d:lines("*L") do
        table.insert(lines, text)
    end
    lu.assertEquals(#lines, 2)
    lu.assertEquals(lines[1], "a\n")
    lu.assertEquals(lines[2], "b\n")
end

function TestDoc:testLinesBytes()
    -- lines(3) = read(3) each iteration
    local d = pt.doc("abcdefghi")
    local chunks = {}
    for text in d:lines(3) do
        table.insert(chunks, text)
    end
    lu.assertEquals(#chunks, 3)
    lu.assertEquals(chunks[1], "abc")
    lu.assertEquals(chunks[2], "def")
    lu.assertEquals(chunks[3], "ghi")
end

function TestDoc:testReadNoArg()
    -- read() with no args = read line without \n (like io.read)
    local d = pt.doc("hello\nworld")
    lu.assertEquals(d:read(), "hello")
    lu.assertEquals(d:read(), "world")
    lu.assertNil(d:read())
end

function TestDoc:testReadlineMultiPiece()
    -- append+edit+append: [aa(lit)]+[bb(hole)]+[cc\n(lit)]
    local d = pt.doc("")
    d:append("aa"); d:edit(0, "bb"); d:append("cc\n")
    d:seek(0)
    lu.assertEquals(d:read("l"), "aabbcc")
    lu.assertNil(d:read("l"))
end

function TestDoc:testReadlineMultiPiece2()
    -- \n mid-piece, line continues: [aa]+[bb]+[cc\ndd]
    local d = pt.doc("")
    d:append("aa"); d:edit(0, "bb"); d:append("cc\ndd")
    d:seek(0)
    lu.assertEquals(d:read("l"), "aabbcc")
    lu.assertEquals(d:read("l"), "dd")
end

function TestDoc:testReadlineMultiPieceEmptyLine()
    -- double \n in hole: [aa]+[\n\n]+[bb]
    local d = pt.doc("")
    d:append("aa"); d:edit(0, "\n\n"); d:append("bb")
    d:seek(0)
    lu.assertEquals(d:read("l"), "aa")
    lu.assertEquals(d:read("l"), "")
    lu.assertEquals(d:read("l"), "bb")
    lu.assertNil(d:read("l"))
end

function TestDoc:testReadlineMultiPieceWithNL()
    -- read("L") across pieces: [aa]+[bb]+[cc\n]
    local d = pt.doc("")
    d:append("aa"); d:edit(0, "bb"); d:append("cc\n")
    d:seek(0)
    lu.assertEquals(d:read("L"), "aabbcc\n")
    lu.assertNil(d:read("L"))
end

function TestDoc:testReadlineMultiPieceNoNewline()
    -- 3 pieces, no \n, EOF nil
    local d = pt.doc("")
    d:append("aa"); d:edit(0, "bb"); d:append("cc")
    d:seek(0)
    lu.assertEquals(d:read("l"), "aabbcc")
    lu.assertNil(d:read("l"))
end

function TestDoc:testReadlineMultiPieceSeekMiddle()
    -- seek mid-piece, cross boundary to \n in later piece
    local d = pt.doc("")
    d:append("aa"); d:edit(0, "bb"); d:append("cc\n")
    d:seek(1)
    lu.assertEquals(d:read("l"), "abbcc")
end

function TestDoc:testLinesMultiPiece()
    -- lines() across append/edit/append/edit/append pieces
    local d = pt.doc("")
    d:append("aa\n"); d:edit(0, "bb"); d:append("\n"); d:edit(0, "cc")
    d:seek(0)
    local t = {}
    for s in d:lines() do table.insert(t, s) end
    lu.assertEquals(#t, 3)
    lu.assertEquals(t[1], "aa")
    lu.assertEquals(t[2], "bb")
    lu.assertEquals(t[3], "cc")
end

function TestDoc:testPieceBoundaryNewline()
    -- [abc](lit)+[def](hole)+[zzz\n](lit): \n crosses piece boundary
    local d = pt.doc("")
    d:append("1231231234")
    lu.assertEquals(d:breaks(), 1)
    d:seek(0); d:remove(10)
    d:append("abc")
    d:edit(0, "def")
    d:append("zzz\n")
    lu.assertEquals(d:dump(), "abcdefzzz\n")
    lu.assertEquals(d:breaks(), 1)
    lu.assertEquals(d:linelen(0), 10)
end

function TestDoc:testScannerCrossPiece()
    local d = pt.doc("abc")
    d:seek("set", 3)
    d:write("def")
    d:write("ghi")
    d:seek("set", 0)
    -- Trigger lc_scan with UNL end → crosses piece boundaries
    lu.assertEquals(d:breaks(), 1)
    lu.assertEquals(d:dump(), "abcdefghi")
end

function TestDoc:testScannerNewlineInPiece()
    local d = pt.doc("hello\nworld")
    d:seek("end")
    d:write("\nmore")
    lu.assertEquals(d:breaks(), 3)
end

-- Buffer delete + error tests

function TestBuffer:testDeleteError()
    local b = pt.from("hi")
    b:delete()
    lu.assertErrorMsgContains("invalid Buffer",
        function() b:read(0) end)
    lu.assertErrorMsgContains("invalid Buffer",
        function() b:cursor(0) end)
end

-- Negative arg triggers

function TestBuffer:testReadNegLen()
    local b = pt.from("hi")
    lu.assertErrorMsgContains("length must be non-negative",
        function() b:read(0, -5) end)
end

function TestCursor:testReadNegLen()
    local c = pt.from("hi"):cursor(0)
    lu.assertErrorMsgContains("length must be non-negative",
        function() c:read(-1) end)
end

function TestDoc:testUndoCursorPosition()
    local d = pt.doc("hello")
    d:commit()
    d:seek("set", 3) -- at second 'l'
    d:write("xxx")   -- insert "xxx" at pos 3
    d:commit()       -- "helxxxlo", cursor at 6
    d:undo()         -- back to "hello"; cursor should map to edit origin
    lu.assertEquals(d:dump(), "hello")
    lu.assertEquals(d:offset(), 3)
end

function TestDoc:testRedoCursorPosition()
    local d = pt.doc("hello")
    d:commit()
    d:seek("set", 3)
    d:write("xxx")
    d:commit() -- "helxxxlo", cursor at 6
    d:undo()   -- back to "hello", cursor at 3
    d:redo()   -- forward to "helxxxlo"
    lu.assertEquals(d:dump(), "helxxxlo")
    lu.assertEquals(d:offset(), 3) -- insert point (geometric mapping)
end

function TestDoc:testFreshUndoCursorPosition()
    local d = pt.doc("hello")
    d:commit()
    d:seek("set", 2)
    d:write("yyy") -- "heyyyllo", cursor at 5, uncommitted
    d:undo()       -- discard fresh edits
    lu.assertEquals(d:dump(), "hello")
    lu.assertEquals(d:offset(), 2)
end

function TestDoc:testUndoLinecacheConsistency()
    local d = pt.doc("line0\nline1\nline2\nline3\nline4\n")
    d:seek("line", 2); d:seek("cur", 1); d:edit(0, "X"); d:commit()
    d:seek("line", 1); local ll = d:linelen(1); d:remove(ll)
    d:undo()
    lu.assertEquals(d:dump(), "line0\nline1\nline2\nline3\nline4\n")
    lu.assertEquals(d:breaks(), 5)
    d:seek("set", 0)
    local yielded = {}
    for text in d:lines() do table.insert(yielded, text) end
    lu.assertEquals(#yielded, 5)
    lu.assertEquals(yielded[5], "line4")
end

function TestDoc:testNewBadArg()
    lu.assertErrorMsgContains("expected nil, Buffer, or string",
        ---@diagnostic disable-next-line: param-type-mismatch
        function() pt.doc(true) end)
end

function TestDoc:testReadBadArg()
    local d = pt.doc("hi")
    lu.assertErrorMsgContains("string expected",
        ---@diagnostic disable-next-line: param-type-mismatch
        function() d:read({}) end)
end

function TestDoc:testReadBadFormat()
    local d = pt.doc("hi")
    lu.assertErrorMsgContains("invalid format",
        ---@diagnostic disable-next-line: param-type-mismatch
        function() d:read("x") end)
end

function TestDoc:testSeekLineToBreaks()
    -- doc "hello\nworld": breaks=1, seek("line", 0) = line 0 start
    -- seek("line", 1) = residual row start (lnum == breaks)
    local d = pt.doc("hello\nworld")
    d:seek("line", 0)
    lu.assertEquals(d:offset(), 0)
    lu.assertEquals(d:read(5), "hello")
    d:seek("line", 1)
    lu.assertEquals(d:offset(), 6)
    lu.assertEquals(d:read("a"), "world")
end

function TestDoc:testLineLenTrailing()
    -- trailing line: doc "ab" has no \n, linelen() at cursor = 2
    local d = pt.doc("ab")
    lu.assertEquals(d:linelen(0), 2) -- arg form
    d:seek("set", 0)
    lu.assertEquals(d:linelen(), 2)  -- current line at trailing
end

-- branch coverage: negative offset / empty-string errors

function TestBuffer:testReadNegOffset()
    local b = pt.from("hi")
    lu.assertErrorMsgContains("offset must be non-negative",
        function() b:read(-1) end)
end

function TestCursor:testLocateNeg()
    local c = pt.from("hi"):cursor(0)
    lu.assertErrorMsgContains("offset must be non-negative",
        function() c:locate(-1) end)
end

function TestDoc:testEditTooLong()
    local d = pt.doc("hello")
    d:seek("set", 1)
    lu.assertErrorMsgContains("string too long for hole",
        function() d:edit(0, string.rep("x", 65)) end)
    lu.assertEquals(d:dump(), "hello") -- unchanged after error
end

function TestDoc:testErrorAndRecover()
    local d = pt.doc("hello world")
    -- edit with oversized hole string
    lu.assertErrorMsgContains("string too long for hole",
        function() d:edit(0, string.rep("x", 65)) end)
    lu.assertEquals(d:dump(), "hello world")
    -- commit no-op still works
    d:commit()
    d:seek("set", 0)
    lu.assertEquals(d:read("a"), "hello world")
end

function TestCursor:testEmptyStringOps()
    local c = pt.from("hello"):cursor(3)
    c:insert("")
    c:append("")
    c:splice(0, "")
    lu.assertEquals(c:offset(), 3)
end

function TestCursor:testErrorPaths()
    local c = pt.from("hi"):cursor(0)
    -- edit with oversized hole string
    lu.assertErrorMsgContains("bad argument",
        function() c:edit(0, string.rep("x", 65)) end)
    lu.assertEquals(c:offset(), 0)
end

function TestCursor:testDetachMore()
    local b = pt.from("original")
    local c = b:cursor(0)
    c:commit()
    lu.assertErrorMsgContains("invalid Cursor", function() c:read(1) end)
    lu.assertErrorMsgContains("invalid Cursor", function() c:offset() end)
    lu.assertErrorMsgContains("invalid Cursor", function() c:locate(0) end)
end

-- Buffer error paths

function TestDoc:testNewNil()
    local d = pt.doc(nil)
    lu.assertEquals(#d, 0)
end

function TestDoc:testSeekNumericNeg()
    local d = pt.doc("hi")
    lu.assertErrorMsgContains("offset must be non-negative",
        function() d:seek(-1) end)
end

function TestDoc:testEmptyEdits()
    local d = pt.doc("hello")
    d:insert(""):write(""):splice(0, "")
    lu.assertEquals(d:dump(), "hello")
end

function TestDoc:testSeekLineOutOfRange()
    local d = pt.doc("a\nb")
    lu.assertErrorMsgContains("line out of range",
        function() d:seek("line", 999) end)
end

function TestDoc:testLineLenOutOfRange()
    local d = pt.doc("a\nb")
    lu.assertErrorMsgContains("line number out of range",
        function() d:linelen(999) end)
end

function TestDoc:testUndoAtRoot()
    local d = pt.doc("hello")
    -- undo at root (no parent) is no-op
    local v = d:undo()
    lu.assertEquals(v, d:commit())
    lu.assertEquals(d:dump(), "hello")
end

function TestDoc:testBufferInvalidVid()
    local d = pt.doc("hello")
    d:commit()
    lu.assertErrorMsgContains("invalid vid",
        function() d:buffer(99999) end)
end

function TestDoc:testUndoInvalidVid()
    local d = pt.doc("hello")
    d:commit()
    lu.assertErrorMsgContains("invalid vid",
        function() d:undo(99999) end)
end

-- earlier / later time-travel

function TestDoc:testEariler()
    local d = pt.doc("")
    d:commit()
    d:seek("end"); d:write("A"); local vA = d:commit()
    d:seek("end"); d:write("B"); local vB = d:commit()
    d:seek("end"); d:write("C"); local vC = d:commit()
    -- cursor at end of "ABC" (offset=3)
    local vid = d:earlier() -- -> "AB"
    lu.assertEquals(vid, vB)
    lu.assertEquals(d:dump(), "AB")
    lu.assertEquals(d:offset(), 2)
    vid = d:earlier() -- -> "A"
    lu.assertEquals(vid, vA)
    lu.assertEquals(d:dump(), "A")
    lu.assertEquals(d:offset(), 1)
end

function TestDoc:testLater()
    local d = pt.doc("")
    d:commit()
    d:seek("end"); d:write("A"); local vA = d:commit()
    d:seek("end"); d:write("B"); local vB = d:commit()
    d:seek("end"); d:write("C"); local vC = d:commit()
    d:earlier(); d:earlier() -- at "A", offset=1
    local vid = d:later()    -- -> "AB"
    lu.assertEquals(vid, vB)
    lu.assertEquals(d:dump(), "AB")
    lu.assertEquals(d:offset(), 1)
    vid = d:later() -- -> "ABC"
    lu.assertEquals(vid, vC)
    lu.assertEquals(d:dump(), "ABC")
    lu.assertEquals(d:offset(), 1)
end

function TestDoc:testEarilerAtRoot()
    local d = pt.doc("hello")
    d:commit()
    local vid = d:earlier()
    lu.assertEquals(vid, d:commit())
    lu.assertEquals(d:dump(), "hello")
end

function TestDoc:testLaterAtNewest()
    local d = pt.doc("hello")
    d:commit()
    local vid = d:later()
    lu.assertEquals(vid, d:commit())
    lu.assertEquals(d:dump(), "hello")
end

function TestDoc:testEarilerFreshError()
    local d = pt.doc("hello")
    d:seek("end"); d:write(" world")
    lu.assertErrorMsgContains("uncommitted",
        function() d:earlier() end)
end

function TestDoc:testLaterFreshError()
    local d = pt.doc("hello")
    d:seek("end"); d:write(" world")
    lu.assertErrorMsgContains("uncommitted",
        function() d:later() end)
end

function TestDoc:testTreeEarilerLater()
    -- tree: "" → "A" → "AB" → "ABC", then undo to "AB", branch "ABD"
    -- chronological order: "", "A", "AB", "ABC", "ABD"
    local d = pt.doc("")
    d:commit()
    d:write("A"); local vA = d:commit() -- "A"
    d:write("B"); local vB = d:commit() -- "AB"
    d:write("C"); local vC = d:commit() -- "ABC"
    d:undo()                            -- back to "AB"
    d:write("D"); local vD = d:commit() -- "ABD"
    lu.assertEquals(d:dump(), "ABD")
    lu.assertEquals(d:offset(), 3)
    -- earlier: vD → vC (time order, cross-branch)
    local vid = d:earlier()
    lu.assertEquals(vid, vC)
    lu.assertEquals(d:dump(), "ABC")
    lu.assertEquals(d:offset(), 3)
    -- earlier: vC → vB
    vid = d:earlier()
    lu.assertEquals(vid, vB)
    lu.assertEquals(d:dump(), "AB")
    lu.assertEquals(d:offset(), 2)
    -- later: vB → vC
    vid = d:later()
    lu.assertEquals(vid, vC)
    lu.assertEquals(d:dump(), "ABC")
    lu.assertEquals(d:offset(), 2)
    -- later: vC → vD
    vid = d:later()
    lu.assertEquals(vid, vD)
    lu.assertEquals(d:dump(), "ABD")
    lu.assertEquals(d:offset(), 2)
    -- navigate to root: vD→vB(undo), vB→vA(earlier), vA→root(earlier)
    d:undo(); d:earlier(); d:earlier()
    lu.assertEquals(d:dump(), "")
    vid = d:earlier()
    lu.assertEquals(vid, d:commit())
    lu.assertEquals(d:dump(), "")
    -- at newest (vD), later is no-op
    d:undo(vD)
    lu.assertEquals(d:dump(), "ABD")
    vid = d:later()
    lu.assertEquals(vid, d:commit())
    lu.assertEquals(d:dump(), "ABD")
end

-- Piece tests

function TestDoc:testPieceLengthInvariant()
    -- piece("len") is read-only, does not move cursor
    local d = pt.doc("hello\nworld")
    d:seek("set", 0)
    local l1 = d:piece("len")
    lu.assertNotEquals(l1, 0)
    local off1 = d:offset()
    local l2 = d:piece("len")
    lu.assertEquals(l2, l1)
    lu.assertEquals(d:offset(), off1)
end

function TestDoc:testPieceNextSum()
    -- sum of all piece lengths = doc length
    local d = pt.doc("hello")
    d:seek("set", 0)
    local total = d:piece("len")
    local next = d:piece("next")
    while next > 0 do
        total = total + next
        next = d:piece("next")
    end
    lu.assertEquals(total, 5)
end

function TestDoc:testPieceNextLast()
    -- next from last piece = 0
    local d = pt.doc("hello")
    d:seek("end")
    lu.assertEquals(d:piece("next"), 0)
    -- cursor at end, piece len = 0
    lu.assertEquals(d:piece("len"), 0)
end

function TestDoc:testPiecePrevFirst()
    -- prev from first piece = 0
    local d = pt.doc("hello")
    d:seek("set", 0)
    lu.assertEquals(d:piece("prev"), 0)
end

function TestDoc:testPieceMidRemaining()
    -- cursor mid-piece: len = remaining bytes
    local d = pt.doc("hello")
    d:seek("set", 2)
    local rem = d:piece("len")
    lu.assertEquals(rem, 3)  -- "llo"
    -- next after cursor mid-piece: skips remaining, starts next piece
    local n = d:piece("next")
    lu.assertEquals(rem + n, 3)  -- n=0 since only 1 piece
end

function TestDoc:testPieceSeekMidAdvance()
    -- next from mid-piece skips remaining bytes of current piece
    local d = pt.doc("")
    d:append("aa"); d:edit(0, "bb"); d:append("cc")
    d:seek("set", 2)
    local rem = d:piece("len")
    lu.assertNotEquals(rem, 0)
    local n = d:piece("next")
    lu.assertNotEquals(n, 0)
    -- after advance, cursor at start of next piece: len = full piece
    lu.assertEquals(d:piece("len"), n)
end

function TestDoc:testPieceNextAdvances()
    -- each piece("next") moves cursor forward
    local d = pt.doc("")
    d:append("aa"); d:edit(0, "bb"); d:append("cc")
    d:seek("set", 0)
    local off0 = d:offset()
    d:piece("next")
    local off1 = d:offset()
    lu.assertNotEquals(off1, off0)
    d:piece("next")
    local off2 = d:offset()
    lu.assertNotEquals(off2, off1)
    -- after last piece, next=0 and offset stays at end
    local next2 = d:piece("next")
    lu.assertEquals(next2, 0)
    lu.assertEquals(d:offset(), #d)
end

function TestDoc:testPiecePrevAdvances()
    -- piece("prev") moves cursor backward
    local d = pt.doc("")
    d:append("aa"); d:edit(0, "bb"); d:append("cc")
    d:seek("end")
    local prev = d:piece("prev")
    lu.assertNotEquals(prev, 0)
    -- iterate to first piece: prev=0 means no more before
    local steps = 0
    while prev > 0 do
        steps = steps + 1
        prev = d:piece("prev")
    end
    lu.assertEquals(d:offset(), 0)
    lu.assertNotEquals(steps, 0)
end

function TestDoc:testPieceBoundaryPoff()
    -- cursor at piece boundary: len = full piece (> 0)
    local d = pt.doc("")
    d:append("aa"); d:edit(0, "bb"); d:append("cc")
    d:seek("set", 0)
    lu.assertNotEquals(d:piece("len"), 0)
    d:piece("next")
    lu.assertNotEquals(d:piece("len"), 0)
    -- next advances cursor: prove with offset
    d:seek("set", 0)
    local off0 = d:offset()
    d:piece("next")
    lu.assertNotEquals(d:offset(), off0)
end

function TestDoc:testPieceSingleLoop()
    -- single piece: len > 0, next = 0, prev = 0
    local d = pt.doc("x")
    d:seek("set", 0)
    lu.assertNotEquals(d:piece("len"), 0)
    lu.assertEquals(d:piece("next"), 0)
    -- reseek: prev from start = 0
    d:seek("set", 0)
    lu.assertEquals(d:piece("prev"), 0)
end

os.exit(lu.LuaUnit.run(), true)
