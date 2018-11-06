# luajitgen
将lua5.4的分代移植到luajit中，在jit.off()模式下通过[openresty](https://github.com/openresty/luajit2-test-suite)和[lua官方](https://github.com/LuaJIT/LuaJIT-test-cleanup)测试用例

只提交了src文件夹，在LuaJIT 2.1.0-beta3版本下直接替换src文件夹即可编译通过

lua5.4分代分析，以及luajit移植时候遇到的问题可以查看[https://drecik.top/2018/11/04/18/#more](https://drecik.top/2018/11/04/18/#more)
