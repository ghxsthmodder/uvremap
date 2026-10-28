# uvremap

**uvremap** é uma ferramenta de linha de comando (CLI) para analisar e
regenerar o UV mapping de modelos 3D, transferindo as texturas originais
para o novo atlas. O foco inicial é o formato **glTF/GLB**, usando
**xatlas** para gerar o novo mapeamento UV.

> **Status do projeto:** especificação conceitual inicial (v0.1). Os
> comandos e parâmetros descritos neste documento definem a interface
> proposta; não implicam que a implementação já exista.

## Objetivos

-   Analisar modelos glTF e GLB, incluindo geometria, UV sets, materiais
    e texturas.
-   Gerar um novo UV mapping com xatlas.
-   Transferir a aparência das texturas originais para o novo
    mapeamento.
-   Processar mapas PBR suportados, incluindo normal maps.
-   Permitir escolher como as malhas e os materiais serão agrupados em
    atlas.
-   Exportar o modelo e seus recursos para um diretório de saída.
-   Preservar, sempre que possível, a hierarquia e os dados do modelo
    que não precisem ser alterados.

## Escopo inicial

A primeira versão prioriza modelos estáticos triangulados em glTF/GLB. A
arquitetura deve permitir expandir o suporte para recursos adicionais do
glTF, modelos animados, processamento em lote e integrações com
editores.

A transferência de normal maps exige tratamento próprio: não basta
copiar a imagem antiga para o novo atlas, pois a mudança de
parametrização pode alterar a base tangente. A implementação deverá
rebakear ou transformar corretamente os mapas de normal para o novo UV
mapping. Recursos que ainda não puderem ser processados com segurança
devem gerar avisos explícitos, e não ser silenciosamente alterados.

## Instalação

Cada release publica quatro binários:

| Arquivo | Plataforma |
|---|---|
| `uvremap-<versão>-linux-x86_64.tar.gz` | Linux x86_64 (glibc) |
| `uvremap-<versão>-linux-aarch64.tar.gz` | Linux ARM64 com glibc (Raspberry Pi OS 64-bit, servidores ARM) |
| `uvremap-<versão>-android-arm64.tar.gz` | Android/Termux (arm64-v8a, API 24+) |
| `uvremap-<versão>-windows-x64.zip` | Windows x64 |

No Termux, use o pacote `android-arm64`: os binários Android não têm
glibc, e um binário `linux-*` falhará com `cannot execute: required
file not found`. Extraia e marque como executável:

```sh
tar -xzf uvremap-<versão>-android-arm64.tar.gz
chmod +x uvremap && ./uvremap version
```

## Requisitos e dependências propostas

-   **C++20** --- linguagem e padrão de desenvolvimento.
-   **xatlas** --- geração de charts, parametrização e packing do atlas.
-   **tinygltf** --- candidato para leitura e escrita de glTF/GLB.
-   **stb_image / stb_image_write** --- candidatas para leitura e
    escrita de imagens raster.

Essas bibliotecas são escolhas propostas para a arquitetura inicial. A
seleção final, versões e licenças deverão ser confirmadas durante a
implementação.

## Fluxo de processamento

1.  **Carregar:** abrir o glTF/GLB, resolver recursos referenciados e
    ler geometria, atributos, materiais, UV sets e texturas.
2.  **Validar:** verificar se a entrada contém geometria e UVs de origem
    suficientes para transferir a aparência existente.
3.  **Analisar:** identificar primitivas, materiais, texturas e
    conjuntos UV relevantes.
4.  **Preparar a geometria:** converter os dados para a representação
    exigida pelo xatlas, preservando correspondências com os vértices e
    atributos originais.
5.  **Gerar o atlas:** executar xatlas de acordo com a estratégia de
    agrupamento escolhida.
6.  **Transferir as texturas:** para cada ponto coberto pelo novo atlas,
    localizar a superfície correspondente e amostrar as texturas
    originais por meio dos UVs de origem.
7.  **Atualizar os recursos:** escrever os novos UVs, as imagens
    reconstruídas e as referências de materiais necessárias.
8.  **Exportar:** salvar o modelo e todos os recursos gerados no
    diretório de saída.
9.  **Relatar:** apresentar estatísticas, avisos e erros do
    processamento.

**Responsabilidades:** xatlas gera o novo mapeamento; o uvremap é
responsável por transferir as texturas, atualizar o modelo e exportar os
recursos.

## CLI

A forma geral proposta é:

``` text
uvremap <command> [options] <input>
```

### Comandos

  -----------------------------------------------------------------------
  Comando                             Descrição
  ----------------------------------- -----------------------------------
  `regenerate`                        Gera um novo UV mapping e transfere
                                      as texturas. É o comando principal.

  `scan`                              Analisa o modelo e o UV mapping
                                      existente.

  `inspect`                           Exibe detalhes de primitivas,
                                      materiais, UV sets e imagens.

  `preview`                           Gera imagens de diagnóstico do UV
                                      mapping e do atlas.

  `version`                           Exibe a versão e informações de
                                      compilação.
  -----------------------------------------------------------------------

### `regenerate`

``` text
uvremap regenerate <input> [options]
```

Exemplo básico:

``` bash
uvremap regenerate vehicle.glb \
  --output ./vehicle_remapped
```

O diretório de saída conterá o modelo final e os recursos associados.
Por padrão, o uvremap não deverá sobrescrever arquivos existentes sem
autorização explícita.

### Opções propostas

  -----------------------------------------------------------------------
  Opção                               Descrição
  ----------------------------------- -----------------------------------
  `-o, --output PATH`                 Diretório de saída com o modelo e
                                      os recursos gerados.

  `--atlas-mode MODE`                 Estratégia de agrupamento:
                                      `material`, `mesh` ou `scene`.
                                      Obrigatória para `regenerate` na
                                      especificação inicial, para que a
                                      escolha seja explícita.

  `--resolution WIDTHxHEIGHT`         Resolução do espaço do atlas usado
                                      na geração UV. Qualquer W x H é
                                      aceito: o xatlas empacota em quadrado
                                      e o runner re-escala os charts para
                                      W x H (o padding do pack é ampliado no
                                      eixo comprimido). Se as ilhas não
                                      couberem, o xatlas abre páginas
                                      extras; as texturas finais empilham
                                      as páginas verticalmente (W x H*N).

  `--padding PIXELS`                  Margem entre ilhas UV, expressa em
                                      pixels do atlas.

  `--texels-per-unit VALUE`           Meta de densidade de texels em
                                      relação às unidades do modelo. É
                                      uma meta, não uma garantia.

  `--source-uv INDEX`                 Índice do conjunto UV usado para
                                      amostrar as texturas de origem.

  `--target-uv INDEX`                 Índice do conjunto UV que receberá
                                      o novo mapeamento.

  `--texture-filter MODE`             Filtro de amostragem: `nearest` ou
                                      `linear`.

  `--texture-size WIDTHxHEIGHT`       Resolução das imagens de textura
                                      reconstruídas.

  `--split-materials`                 Emite uma imagem por material por tipo
                                      de mapa, mesmo quando o atlas é
                                      compartilhado. Por padrão um atlas com
                                      vários materiais produz uma única
                                      imagem por tipo (as ilhas de cada
                                      material ocupam regiões disjuntas);
                                      modelos com `KHR_materials_variants`
                                      usam imagens por material de qualquer
                                      forma.

  `--self-check N`                    Amostra N pontos aleatórios por
                                      imagem reconstruída e confere a
                                      concordância com a textura de origem
                                      (percentuais no `report.txt`; mapas
                                      de normal são pulados, pois o rebake
                                      em quadro tangente não é comparável
                                      texel a texel).

  `--format FORMAT`                   Formato do modelo de saída: `gltf`
                                      ou `glb`.

  `--textures MODE`                   Destino das texturas na saída `gltf`:
                                      `external` (arquivos em `textures/`,
                                      padrão) ou `embedded` (base64 dentro
                                      do `.gltf`). Saída `glb` sempre embute
                                      as texturas; `external` com `glb` é
                                      rejeitado.

  `--dry-run`                         Valida a entrada e apresenta o
                                      plano sem gravar os arquivos
                                      finais.

  `--preview`                         Escreve PNGs de diagnóstico no
                                      diretório de saída, por atlas:
                                      `<name>_atlas_preview.png` (layout
                                      das ilhas), `<name>_uv_compare.png`
                                      (wireframe das UVs de origem ao lado
                                      do novo atlas) e
                                      `<name>_heatmaps.png` (ocupação
                                      branco-preto e mapa de calor de
                                      densidade de texels, escala log).

  `--json`                            Escreve `report.json` no diretório
                                      de saída e imprime o relatório
                                      final em JSON na saída padrão
                                      (stats, avisos, informações e
                                      erros) em vez do resumo legível,
                                      para integração em pipelines.

  `--force`                           Autoriza a sobrescrita de arquivos
                                      existentes no destino.

  `--verbose`                         Exibe detalhes adicionais.

  `--quiet`                           Minimiza a saída no terminal.
  -----------------------------------------------------------------------

Os índices de `--source-uv` e `--target-uv` correspondem aos conjuntos
`TEXCOORD_n` do glTF; não são índices de materiais ou texturas.

### Estratégias de atlas: `--atlas-mode`

O usuário deverá selecionar explicitamente uma das três opções:

-   **`material`** --- cria atlas por material. Mantém o agrupamento de
    recursos relacionado a cada material, mas pode produzir vários
    atlas.
-   **`mesh`** --- cria atlas por malha, agrupando primitivas
    compatíveis que pertencem à mesma malha.
-   **`scene`** --- cria atlas para a cena, agrupando a geometria
    elegível em um atlas compartilhado. Pode exigir alterações mais
    abrangentes nos materiais e referências de textura.

Exemplos:

``` bash
# Um atlas por material
uvremap regenerate vehicle.glb \
  --output ./out_material \
  --atlas-mode material

# Um atlas por malha
uvremap regenerate vehicle.glb \
  --output ./out_mesh \
  --atlas-mode mesh

# Um atlas para a cena
uvremap regenerate vehicle.glb \
  --output ./out_scene \
  --atlas-mode scene
```

A compatibilidade de agrupamento deve considerar as diferenças entre
materiais, conjuntos UV, atributos da malha e tipos de textura. A opção
`scene` não deve combinar dados incompatíveis apenas para forçar um
único atlas.

### Exemplos adicionais

Atlas de alta resolução:

``` bash
uvremap regenerate vehicle.glb \
  --output ./vehicle_remapped \
  --atlas-mode mesh \
  --resolution 4096x4096 \
  --padding 8
```

Usar o UV 0 como fonte e gravar o novo mapeamento no UV 1:

``` bash
uvremap regenerate model.gltf \
  --output ./model_remapped \
  --atlas-mode material \
  --source-uv 0 \
  --target-uv 1
```

Validar antes de processar:

``` bash
uvremap regenerate model.glb \
  --output ./model_remapped \
  --atlas-mode scene \
  --dry-run \
  --verbose
```

## Transferência de texturas PBR

A meta é transferir todos os tipos de textura PBR suportados pelo
uvremap, incluindo normal maps. A implementação deve tratar cada tipo
segundo sua semântica, em vez de aplicar uma transformação genérica a
todas as imagens.

  -----------------------------------------------------------------------
  Tipo de textura                     Regra de processamento
  ----------------------------------- -----------------------------------
  Base color / albedo                 Transferir cor e alfa quando
                                      presentes, respeitando o espaço de
                                      cor e o modo alfa do material.

  Normal map                          Rebakear ou transformar levando em
                                      conta a nova parametrização e a
                                      base tangente. Não basta copiar os
                                      pixels antigos.

  Metallic-roughness                  Preservar a semântica dos canais:
                                      roughness no canal G e metallic no
                                      canal B, conforme o glTF.

  Occlusion                           Transferir os valores de oclusão e
                                      respeitar o canal utilizado pelo
                                      material.

  Emissive                            Transferir os valores de emissão,
                                      respeitando a interpretação de cor.

  Outros mapas                        Processar quando houver suporte
                                      explícito; caso contrário, emitir
                                      aviso e preservar a origem sem
                                      alterações destrutivas sempre que
                                      possível.
  -----------------------------------------------------------------------

A transferência deverá considerar também texturas compartilhadas entre
materiais, transformações de textura, canais UV utilizados e parâmetros
específicos dos materiais. Quando uma textura for compartilhada por
recursos que precisam de atlas diferentes, o uvremap poderá precisar
gerar cópias reconstruídas para manter referências corretas.

## Preservação do modelo

O uvremap deverá preservar, sempre que o formato e a operação
permitirem:

-   Hierarquia da cena e transformações dos nós.
-   Materiais e fatores numéricos não relacionados à transferência.
-   Normais, tangentes, cores de vértice e atributos adicionais.
-   Skinning, pesos, morph targets e animações, quando suportados.
-   Referências e recursos que não precisem ser alterados.

O xatlas pode duplicar vértices em descontinuidades UV. Portanto, o
uvremap deverá reconstruir os atributos da malha usando os índices e as
correspondências de vértices fornecidos pelo processo de geração, em vez
de assumir que a topologia indexada original ficará idêntica.

O arquivo de entrada deverá permanecer intacto. A saída será gravada no
diretório indicado por `--output`.

## Organização do diretório de saída

A estrutura exata poderá variar conforme o formato selecionado, mas o
diretório deverá reunir o modelo e todos os recursos que ele referencia.
Uma organização ilustrativa para saída glTF:

``` text
vehicle_remapped/
├── model.gltf
├── textures/
│   ├── material_0_basecolor.png
│   ├── material_0_normal.png
│   ├── material_0_metallic_roughness.png
│   ├── material_0_occlusion.png
│   └── material_0_emissive.png
└── report.txt
```

Para saída GLB, o modelo poderá incorporar os recursos compatíveis no
próprio arquivo, mas o diretório continuará sendo o destino da operação
e poderá conter relatórios ou recursos auxiliares, conforme a
implementação. A saída deve manter referências válidas e não depender
acidentalmente de caminhos absolutos da máquina de origem.

## Diagnóstico e relatório

Exemplo ilustrativo da saída do terminal:

``` text
uvremap 0.1.0

Input:       vehicle.glb
Output:      ./vehicle_remapped
Atlas mode:  mesh

[1/5] Loading model ............. OK
[2/5] Analyzing geometry ........ OK
[3/5] Generating UV atlas ....... OK
[4/5] Baking textures ........... OK
[5/5] Exporting assets .......... OK

Summary:
  Primitives processed: 12
  Atlas resolution:     4096x4096
  Texture maps baked:   4
  Warnings:             0

Completed successfully.
```

Os números são apenas exemplos; a aplicação deverá apresentar valores
medidos durante a execução.

### Códigos de saída propostos

    Código Significado
  -------- ------------------------------------------------------------
       `0` Sucesso.
       `1` Erro interno ou falha inesperada.
       `2` Argumentos inválidos ou opções incompatíveis.
       `3` Falha ao carregar ou interpretar o modelo.
       `4` Geometria ou UVs insuficientes para a operação solicitada.
       `5` Falha na geração do atlas ou na transferência de textura.
       `6` Falha ao gravar os arquivos de saída.

Avisos que não impedem a operação devem ser relatados sem
necessariamente causar falha. O programa não deverá declarar sucesso se
o modelo exportado contiver referências quebradas ou se uma
transferência obrigatória tiver falhado.

## Regras de integridade e segurança de saída

-   Não sobrescrever arquivos existentes sem `--force`.
-   Validar os argumentos e a compatibilidade da entrada antes de
    iniciar a gravação final.
-   Manter o arquivo original intacto.
-   Evitar referências absolutas no modelo exportado.
-   Verificar se as imagens e os buffers referenciados foram gravados
    corretamente.
-   Informar explicitamente recursos não suportados ou mapas que não
    puderam ser transferidos.
-   Sempre que possível, escrever em arquivos temporários e publicar a
    saída final somente após a conclusão bem-sucedida.

## Plano de desenvolvimento

### Fase 1 --- Leitura e diagnóstico

-   Implementar a CLI e o comando `version`.
-   Ler glTF/GLB.
-   Inspecionar malhas, materiais, UV sets e texturas.
-   Implementar `scan` e `inspect`.

### Fase 2 --- Geração UV

-   Integrar xatlas.
-   Implementar `--atlas-mode material|mesh|scene`.
-   Gerar e gravar novos UVs.
-   Preservar os demais atributos da malha.

### Fase 3 --- Transferência de texturas

-   Implementar rasterização e amostragem da textura de origem.
-   Transferir base color e alfa.
-   Implementar os demais mapas PBR suportados, incluindo tratamento
    correto de normal maps.
-   Aplicar padding e filtragem adequados.

### Fase 4 --- Exportação e validação

-   Exportar para diretório de saída.
-   Atualizar materiais e referências.
-   Validar os arquivos gerados.
-   Implementar relatórios, códigos de saída e `--dry-run`.

### Fase 5 --- Robustez

-   Ampliar a preservação de recursos do glTF.
-   Adicionar testes com modelos com múltiplos materiais, UV sets e
    texturas compartilhadas.
-   Avaliar desempenho e uso de memória em modelos maiores.

## Testes essenciais

A suíte de testes deverá incluir:

-   Modelo com uma malha, um material e uma textura.
-   Modelo com múltiplas primitivas e materiais.
-   Modelo com mais de um UV set.
-   Texturas com alfa.
-   Mapas metallic-roughness, occlusion, emissive e normal.
-   Texturas compartilhadas entre materiais.
-   UVs sobrepostos ou fora do intervalo convencional.
-   Arquivos inválidos ou com recursos ausentes.
-   Saída glTF com imagens externas e saída GLB.
-   Verificação visual da aparência antes e depois da transferência.

## Princípios do projeto

1.  **Aparência preservada:** a regeneração deve buscar manter a
    aparência visual original, dentro dos limites de resolução e dos
    algoritmos empregados.
2.  **Operações explícitas:** a estratégia de atlas será escolhida por
    `--atlas-mode`.
3.  **Saída organizada:** o modelo e os recursos gerados ficarão
    reunidos no diretório de saída.
4.  **Preservação de dados:** alterar UVs não deve descartar atributos
    ou recursos não relacionados sem aviso.
5.  **Diagnóstico claro:** erros, avisos e limitações devem ser
    comunicados de maneira útil para usuários e scripts.
6.  **Arquitetura reutilizável:** o núcleo de processamento deverá ser
    independente da CLI para permitir integração futura com editores e
    outras ferramentas.

## Licença

A licença do uvremap ainda deverá ser definida. As licenças das
dependências escolhidas também deverão ser verificadas e documentadas
antes da distribuição.
