Atualização do Spy / Spy Scan — ContaCam

Escopo alterado
- Spy Scan lista candidatos de vários fabricantes/modos no mesmo endereço e porta, em vez de parar no primeiro.
- Inclui modos HTTP MJPEG e snapshot JPEG do catálogo, mais caminhos genéricos HTTP já usados pelo ContaCam.
- HTTP é contado como encontrado somente quando a resposta contém tipo de conteúdo compatível e bytes de início de imagem JPEG.
- A lista do Spy permite selecionar vários fabricantes/protocolos; sem seleção explícita, a varredura percorre os modos disponíveis.
- O Spy automático tenta aprender e salvar um modo/caminho alternativo, incluindo HTTP, porta e RTSP, e consulta essa alternativa durante a recuperação.
- A substituição automática conserva o redirecionamento persistente dos recortes para a câmera substituta.

Limites de validação
- RTSP é validado com resposta positiva a DESCRIBE. Isso confirma que o servidor anuncia o caminho, mas não prova por si só que PLAY/decodificação continuará estável. A confirmação de quadros RTSP após abertura ainda deve ser testada em câmera real antes de considerar a troca totalmente à prova de oscilação.
- HTTP exige recebimento de marcador JPEG real; ainda não mede estabilidade prolongada do MJPEG.
- Somente caminhos e modos implementados/conhecidos pelo ContaCam podem ser descobertos; isso não é um scanner universal de qualquer marca.
- Não foi possível compilar/testar o executável Windows neste ambiente Linux sem Visual Studio/MFC. O ZIP contém o código-fonte para abrir e compilar na solução Windows.
