
# build instructions

```bash
msbuild neverlose.sln /p:Configuration=Release /p:Platform=x86 /p:PlatformToolset=v145
```

## notes

* the toolset might be different on your system
* sometimes you don’t need to specify it at all
* if it fails, just change/remove the toolset in the project settings


## server notes

* make sure your DB is set up (`DATABASE_URL`, etc)
* run migrations before starting the server
* if it doesn’t work first try, it’s usually just an env/toolchain mismatch



## last thing

this whole project — especially the server — has been a massive pain to get working properly

me and spiny nuggie have probably lost a few years of our lives fixing this thing, but it’s finally in a usable state

[https://test.zx9.lol/index.php](https://test.zx9.lol/index.php)

thanks for hitting a peak of **1.6k users** ❤️
means a lot

refer to the LICENSE file for a proper license disclaimer.
