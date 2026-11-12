# Remote360

Remote Play experimental para Xbox 360 RGH/JTAG e Android na rede local. O módulo
captura a imagem do console enquanto o título roda e recebe os comandos dos
controles do Android. O teste físico usou um Xbox 360 Slim com kernel 17559,
Aurora e um AYN Thor.

## Estado do projeto

- O vídeo e os controles embutidos funcionaram em testes anteriores na Aurora e
  no Bully. A estabilidade depende do módulo e do jogo.
- **720p a 60 FPS não foi atingido no console.** Os valores no painel são o pedido
  do aplicativo. O indicador na tela mostra a resolução recebida e os quadros
  desenhados no Android.
- O projeto não transmite áudio e não autentica os pacotes UDP. Use na rede local.

### Medições na Aurora

| Módulo | Perfil solicitado | Resolução entregue | FPS exibidos no Thor |
| --- | --- | --- | ---: |
| snapshot32b | 1280 × 720, 60 FPS | 512 × 288 | 2,3 |
| snapshot32b | 320 × 180, 60 FPS, Priorizar fluidez | 320 × 180 | 19,8 |
| snapshot32c | 1280 × 720, 60 FPS | 512 × 288 | 8,7 |

O Xbox não conseguiu reservar o buffer de captura de 720p. O snapshot32c usa o
processamento paralelo de BC1 ao reduzir a resolução. A medição de 8,7 FPS
incluiu 132 quadros exibidos em 15,17 segundos no catálogo da Aurora.

O snapshot32 carregou na Aurora, mas houve tela preta nas duas tentativas de
abrir Bully com ele ativo. Após reiniciar sem o módulo, Bully abriu normalmente.
Os resultados do snapshot32b e snapshot32c acima são do catálogo da Aurora.

### 0.2.1-rc1

- O módulo `remote360_play.xex` divide a leitura da imagem entre dois núcleos.
  No modo de fluidez, cada núcleo também comprime sua parte em BC1. No modo de
  qualidade, os núcleos codificam faixas JPEG que formam um único quadro.
- Os pacotes de vídeo são intercalados para reduzir a chance de uma rajada
  atingir o mesmo grupo de recuperação várias vezes. O APK tolera pacotes
  atrasados de até um quadro.
- O indicador no vídeo mostra a resolução recebida e os FPS desenhados pelo APK.
  A resolução e os FPS escolhidos no painel são metas. O modo de fluidez tem
  teto de 640 × 360. Para solicitar 720p ou 1080p, desmarque **Priorizar fluidez**.
- Em um trecho estável do Bully no Xbox físico, o modo de fluidez entregou cerca
  de 18,5 FPS a 320 × 180. O teste de 720p não entregou quadros no trecho medido.

## Compilar

O módulo do Xbox usa o OpenXeChain e o SynthXEX com suporte a DLLs de sistema.
Para compilar o módulo da versão candidata, rode:

```sh
OXC=/caminho/openxechain/sysroot bash xbox360/build-video.sh --play
```

O resultado é `xbox360/out/remote360_play.xex`. A opção
`--latency-dual` gera `xbox360/out/remote360_latency_dual2.xex`, usado na medição anterior.
O Android requer JDK 17, SDK 35 e Gradle 8.9.

```sh
gradle -p android :app:assembleDebug :app:testDebugUnitTest :app:lintDebug
python3 -m unittest discover -s tests
```

O APK fica em `android/app/build/outputs/apk/debug/app-debug.apk`. Veja
[BUILD.md](BUILD.md) para detalhes das ferramentas e da validação.

## Testar

1. Reinicie o Xbox na Aurora para descarregar qualquer versão anterior. Abra
   o gerenciador de plugins, marque apenas `remote360_play.xex` e aplique.
   O XEX do módulo precisa do gerenciador de plugins para carregar.
2. Instale o APK no Android. Informe o IP local do Xbox e toque em Conectar.
3. Abra a imagem com 320 × 180, 60 FPS e **Priorizar fluidez**. A opção de 60 FPS
   reconstrói linhas alternadas para reduzir o custo da captura neste módulo.
4. Confira a resolução e os FPS desenhados no app. Repita na Aurora e em um
   jogo em movimento. Para testar 720p, volte ao painel e selecione
   1280 × 720, 60 FPS e deixe **Priorizar fluidez** desmarcado.
5. Com o XBDM disponível, registre os tempos da captura, compressão e envio
   junto dos quadros desenhados no Android:

```sh
python3 tools/watch_xbox.py IP_DO_XBOX play --seconds 60 --summary
```

Em caso de conflito com outro plugin de controle, consulte o status do app.
O teste físico do Thor usou o Remote360 sem o `hiddriver.xex` carregado.
Para voltar ao módulo anterior, reinicie e carregue o XEX anterior no gerenciador.
O [protocolo UDP](protocol/PROTOCOL.md) descreve vídeo, controles e vibração.

## Licença

GPL-3.0-or-later. Consulte [LICENSE](LICENSE) e [THIRD_PARTY.md](THIRD_PARTY.md).
