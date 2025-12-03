Package: nlohmann-json:x64-windows@3.12.0#1

**Host Environment**

- Host: x64-windows
- Compiler: MSVC 19.44.35221.0
- CMake Version: 3.31.6-msvc6
-    vcpkg-tool version: 2025-11-19-da1f056dc0775ac651bea7e3fbbf4066146a55f3
    vcpkg-scripts version: 1a361fe5de 2025-12-02 (5 hours ago)

**To Reproduce**

`vcpkg install `

**Failure logs**

```
Downloading https://github.com/nlohmann/json/archive/v3.12.0.tar.gz -> nlohmann-json-v3.12.0.tar.gz
Successfully downloaded nlohmann-json-v3.12.0.tar.gz

```

**Additional context**

<details><summary>vcpkg.json</summary>

```
{
  "dependencies": [
    "fmt",
    "nlohmann-json"
  ]
}

```
</details>
