TO DO FINAL

- [X] Contraseña WIFI se envía mal
- [X] Afinar deep sleep TCP (sencillo - medio)
    - [X] Al parecer TCP al recibir datos no tiene timeout
    - [X] No llega ack al pasar de UDP a TCP
    - [X] Gran problema: al estar en deep sleep y cambiar de protocolo, no se notifica nunca a 
- [X] Afinar deep sleep MQTT (sencillo - medio): casi listo, solo enviar un flag para indicar ds
- [ ] Afinar deep sleep BLE (está casi listo, implementar idea de flag solamente! igual que tcp para reiniciar conexión)
- [X] Recepción de datos reales de sensores (difícil - parece que no tanto)

- [ ] Si no hay STA, establecer AP
- [ ] MicroSD
- [ ] Recepción parámetros de configuración
- [ ] Heartbeat a base de datos
- [ ] Resolver tema Data_1

- [ ] Depuración de archivos
- [X] README's
- [ ] BME688 no mide bien gas
- [ ] AWS
- [ ] Leds

Restricciones para aplicar configuraciones de sensores on the go:
- Al cambiar de protocolo en caliente hay que de-inicializar o resetear los sensores, a priori
dentro del while
- La inicialización se debe hacer después de la primera conexión BLE, con la config recibida
- En caso de deep sleep, aplicar configuración guardada

Parámetros actuales de los sensores:

BMM350: freq (odr), avg (oversampling, promedio)
BMI270: freq_aceleracion, avg_aceleracion, rango_aceleracion, freq_giroscopio, rango_giroscopio
BME688: temp_ovs/avg, press_ovs/avg, hum_ovs