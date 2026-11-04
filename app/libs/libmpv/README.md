# libmpv 放置目录

这个目录不提交二进制。按下面步骤放好后即可编译：

1. 打开 https://github.com/shinchiro/mpv-winbuild-cmake/releases
2. 下载最新的 `mpv-dev-x86_64-YYYYMMDD-git-XXXXXXXX.7z`（约 30MB）
3. 解压后把内容放成：

```
app/libs/libmpv/
├── libmpv-2.dll
└── include/
    └── mpv/
        ├── client.h
        ├── render.h
        ├── render_gl.h
        └── stream_cb.h
```

注意：
- 只要 x86_64 版本（不要 -v3，除非确定目标机器支持 AVX2）。
- 这个包的 `libmpv.dll.a` 是 MinGW 用的；我们用 QLibrary 动态加载 `libmpv-2.dll`，**不需要链接**，MSVC / MinGW 都能用。
- 升级 libmpv 只需替换 dll（和头文件），代码不用改。
