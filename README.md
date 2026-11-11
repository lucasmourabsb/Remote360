# Remote360

Remote Play experimental para Xbox 360 RGH/JTAG e Android na rede local. O módulo
captura a imagem do console enquanto o título roda e recebe os comandos dos
controles do Android. O teste físico usou um Xbox 360 Slim com kernel 17559,
Aurora e um AYN Thor.

## Estado do projeto

- Vídeo e controles embutidos testados na Aurora e no Bully.
- O perfil de fluidez entregou aproximadamente 12 FPS no Thor a 320 × 180
  durante a navegação na Aurora. Selecionar 60 FPS define o teto solicitado.
- A versão experimental com dois núcleos atingiu 14,1 FPS nas mesmas condições.
  A mediana da compressão caiu de 32,8 ms para 16,6 ms por quadro. A captura
  continuou perto de 34 ms. O teste dessa versão foi feito na Aurora.
- O projeto não transmite áudio e não autentica os pacotes UDP. Use na rede local.

## Compilar

O módulo do Xbox usa o OpenXeChain e o SynthXEX com suporte a DLLs de sistema.
Para reproduzir o teste de fluidez, compile com:

```sh
OXC=/caminho/openxechain/sysroot bash xbox360/build-video.sh --latency-scan
```

O resultado é `xbox360/out/remote360_latency_scan.xex`. A opção
`--latency-dual` gera `xbox360/out/remote360_latency_dual2.xex`.
O Android requer JDK 17, SDK 35 e Gradle 8.9.

```sh
gradle -p android :app:assembleDebug :app:testDebugUnitTest :app:lintDebug
python3 -m unittest discover -s tests
```

O APK fica em `android/app/build/outputs/apk/debug/app-debug.apk`. Veja
[BUILD.md](BUILD.md) para detalhes das ferramentas e da validação.

## Testar

1. Use um gerenciador de plugins que carregue DLLs de sistema. Carregue um único
   módulo Remote360 por vez, sem executar o XEX do módulo diretamente como jogo.
2. Instale o APK no Android. Informe o IP local do Xbox e toque em Conectar.
3. Abra a imagem com 320 × 180, 60 FPS e **Priorizar fluidez**. A opção de 60 FPS
   reconstrói linhas alternadas para reduzir o custo da captura neste módulo.
4. Com o XBDM disponível, meça os quadros que o Android informa ter mostrado:

```sh
python3 tools/watch_xbox.py IP_DO_XBOX latency-scan --seconds 30
python3 tools/watch_xbox.py IP_DO_XBOX latency-dual2 --seconds 30
```

Em caso de conflito com outro plugin de controle, consulte o status do app.
O teste físico do Thor usou o Remote360 sem o `hiddriver.xex` carregado.
O [protocolo UDP](protocol/PROTOCOL.md) descreve vídeo, controles e vibração.

## Licença

GPL-3.0-or-later. Consulte [LICENSE](LICENSE) e [THIRD_PARTY.md](THIRD_PARTY.md).
